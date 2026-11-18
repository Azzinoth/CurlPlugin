#include "CurlPlugin.h"
#include <cstring>

// DO NOT CHANGE THIS LINE.
// The plugin manager uses it to create and destroy the plugin instance.
REGISTER_PLUGIN(CurlPlugin)

static std::wstring ToWidePath(const std::string& Path)
{
	const int WideLength = MultiByteToWideChar(CP_UTF8, 0, Path.c_str(), -1, nullptr, 0);
	std::wstring WidePath(WideLength > 0 ? WideLength - 1 : 0, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, Path.c_str(), -1, WidePath.data(), WideLength);
	return WidePath;
}

static long SecondsToMilliseconds(double Seconds)
{
	if (Seconds <= 0.0)
		return 0L;

	return static_cast<long>(Seconds * 1000.0 + 0.5);
}

static const char* GetMethodName(CurlPluginInterface::HTTPMethod Method)
{
	switch (Method)
	{
		case CurlPluginInterface::HTTPMethod::Get: return "GET";
		case CurlPluginInterface::HTTPMethod::Post: return "POST";
		case CurlPluginInterface::HTTPMethod::Put: return "PUT";
		case CurlPluginInterface::HTTPMethod::Patch: return "PATCH";
		case CurlPluginInterface::HTTPMethod::Delete: return "DELETE";
		case CurlPluginInterface::HTTPMethod::Head: return "HEAD";
	}

	return "GET";
}

bool CurlPlugin::OnLoad()
{
	const CURLcode InitializationResult = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (InitializationResult != CURLE_OK)
	{
		LOG.Add("CurlPlugin::OnLoad: curl_global_init failed: " + std::string(curl_easy_strerror(InitializationResult)), "CURL_PLUGIN", FE_LOG_ERROR);
		return false;
	}

	MultiHandle = curl_multi_init();
	if (MultiHandle == nullptr)
	{
		LOG.Add("CurlPlugin::OnLoad: curl_multi_init failed", "CURL_PLUGIN", FE_LOG_ERROR);
		curl_global_cleanup();
		return false;
	}

	bStopWorker = false;
	WorkerThread = std::thread(&CurlPlugin::RunWorker, this);
	return true;
}

void CurlPlugin::OnUnload()
{
	bStopWorker = true;
	curl_multi_wakeup(MultiHandle);
	if (WorkerThread.joinable())
		WorkerThread.join();

	{
		std::lock_guard<std::mutex> Lock(TransfersMutex);
		for (const std::shared_ptr<Transfer>& NotStartedTransfer : TransfersToStart)
			ReleaseTransferResources(*NotStartedTransfer);

		TransfersToStart.clear();
		RequestIDsToCancel.clear();
		FinishedTransfers.clear();
		ActiveTransfers.clear();
	}

	curl_multi_cleanup(MultiHandle);
	MultiHandle = nullptr;
	curl_global_cleanup();
}

void CurlPlugin::OnUpdate(double DeltaTime)
{
	std::vector<std::shared_ptr<Transfer>> TransfersWithProgress;
	std::vector<std::shared_ptr<Transfer>> TransfersToReport;
	{
		std::lock_guard<std::mutex> Lock(TransfersMutex);
		TransfersToReport.swap(FinishedTransfers);

		for (const std::pair<const uint64_t, std::shared_ptr<Transfer>>& ActiveTransfer : ActiveTransfers)
		{
			if (ActiveTransfer.second->RequestData.OnProgress && ActiveTransfer.second->bProgressChanged.exchange(false))
				TransfersWithProgress.push_back(ActiveTransfer.second);
		}
	}

	// Callbacks could cancel other requests, so every request is checked right before its callback.
	for (const std::shared_ptr<Transfer>& CurrentTransfer : TransfersWithProgress)
	{
		if (IsRequestActive(CurrentTransfer->RequestID))
			ReportProgress(*CurrentTransfer);
	}

	for (const std::shared_ptr<Transfer>& FinishedTransfer : TransfersToReport)
	{
		if (!RemoveActiveTransfer(FinishedTransfer->RequestID))
			continue;

		if (FinishedTransfer->RequestData.OnProgress && FinishedTransfer->bProgressChanged.exchange(false))
			ReportProgress(*FinishedTransfer);

		if (FinishedTransfer->OnResponse)
			FinishedTransfer->OnResponse(FinishedTransfer->Result);
	}
}

void CurlPlugin::ReportProgress(Transfer& CurrentTransfer)
{
	CurrentTransfer.RequestData.OnProgress(CurrentTransfer.DownloadedBytes, CurrentTransfer.TotalBytesToDownload, CurrentTransfer.UploadedBytes, CurrentTransfer.TotalBytesToUpload);
}

size_t CurlPlugin::WriteResponseData(char* Data, size_t Size, size_t Count, void* UserData)
{
	Transfer* CurrentTransfer = static_cast<Transfer*>(UserData);
	const size_t ByteCount = Size * Count;

	// Returning less than ByteCount makes curl fail transfer with write error.
	if (CurrentTransfer->DownloadFileHandle != nullptr)
		return fwrite(Data, 1, ByteCount, CurrentTransfer->DownloadFileHandle);

	CurrentTransfer->Result.Body.append(Data, ByteCount);
	return ByteCount;
}

int CurlPlugin::UpdateTransferProgress(void* UserData, curl_off_t TotalBytesToDownload, curl_off_t DownloadedBytes, curl_off_t TotalBytesToUpload, curl_off_t UploadedBytes)
{
	Transfer* CurrentTransfer = static_cast<Transfer*>(UserData);

	// curl calls it at least once per second, even if nothing changed.
	if (CurrentTransfer->DownloadedBytes == static_cast<uint64_t>(DownloadedBytes) &&
		CurrentTransfer->TotalBytesToDownload == static_cast<uint64_t>(TotalBytesToDownload) &&
		CurrentTransfer->UploadedBytes == static_cast<uint64_t>(UploadedBytes) &&
		CurrentTransfer->TotalBytesToUpload == static_cast<uint64_t>(TotalBytesToUpload))
		return 0;

	CurrentTransfer->DownloadedBytes = static_cast<uint64_t>(DownloadedBytes);
	CurrentTransfer->TotalBytesToDownload = static_cast<uint64_t>(TotalBytesToDownload);
	CurrentTransfer->UploadedBytes = static_cast<uint64_t>(UploadedBytes);
	CurrentTransfer->TotalBytesToUpload = static_cast<uint64_t>(TotalBytesToUpload);

	if (CurrentTransfer->bBlocking)
	{
		CurrentTransfer->RequestData.OnProgress(CurrentTransfer->DownloadedBytes, CurrentTransfer->TotalBytesToDownload, CurrentTransfer->UploadedBytes, CurrentTransfer->TotalBytesToUpload);
	}
	else
	{
		CurrentTransfer->bProgressChanged = true;
	}

	return 0;
}

bool CurlPlugin::PrepareTransfer(Transfer& NewTransfer)
{
	const Request& RequestData = NewTransfer.RequestData;
	Response& Result = NewTransfer.Result;
	Result.RequestID = NewTransfer.RequestID;

	if (RequestData.URL.empty())
	{
		Result.ErrorMessage = "URL is empty";
		return false;
	}

	NewTransfer.EasyHandle = curl_easy_init();
	if (NewTransfer.EasyHandle == nullptr)
	{
		Result.ErrorMessage = "curl_easy_init failed";
		return false;
	}

	CURL* EasyHandle = NewTransfer.EasyHandle;
	curl_easy_setopt(EasyHandle, CURLOPT_URL, RequestData.URL.c_str());
	curl_easy_setopt(EasyHandle, CURLOPT_PRIVATE, &NewTransfer);
	curl_easy_setopt(EasyHandle, CURLOPT_ERRORBUFFER, NewTransfer.ErrorBuffer);
	curl_easy_setopt(EasyHandle, CURLOPT_WRITEFUNCTION, WriteResponseData);
	curl_easy_setopt(EasyHandle, CURLOPT_WRITEDATA, &NewTransfer);
	// With CURLFOLLOW_OBEYCODE PUT, PATCH and DELETE become GET after 301, 302 and 303 redirects, as in browsers.
	curl_easy_setopt(EasyHandle, CURLOPT_FOLLOWLOCATION, RequestData.bFollowRedirects ? CURLFOLLOW_OBEYCODE : 0L);
	curl_easy_setopt(EasyHandle, CURLOPT_CONNECTTIMEOUT_MS, SecondsToMilliseconds(RequestData.ConnectTimeoutInSeconds));
	curl_easy_setopt(EasyHandle, CURLOPT_TIMEOUT_MS, SecondsToMilliseconds(RequestData.TimeoutInSeconds));
	// Schannel fails when revocation could not be checked, for example when firewall blocks revocation servers.
	// Certificates that are known to be revoked are still rejected.
	curl_easy_setopt(EasyHandle, CURLOPT_SSL_OPTIONS, CURLSSLOPT_REVOKE_BEST_EFFORT);

	if (!RequestData.bVerifyCertificate)
	{
		curl_easy_setopt(EasyHandle, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(EasyHandle, CURLOPT_SSL_VERIFYHOST, 0L);
	}

	if (!RequestData.UserAgent.empty())
		curl_easy_setopt(EasyHandle, CURLOPT_USERAGENT, RequestData.UserAgent.c_str());

	if (!RequestData.Proxy.empty())
		curl_easy_setopt(EasyHandle, CURLOPT_PROXY, RequestData.Proxy.c_str());

	if (!RequestData.UserName.empty() || !RequestData.Password.empty())
	{
		curl_easy_setopt(EasyHandle, CURLOPT_USERNAME, RequestData.UserName.c_str());
		curl_easy_setopt(EasyHandle, CURLOPT_PASSWORD, RequestData.Password.c_str());
	}

	for (const std::pair<std::string, std::string>& Header : RequestData.Headers)
	{
		// "Name:" without value tells curl to remove that header.
		const std::string HeaderLine = Header.second.empty() ? Header.first + ":" : Header.first + ": " + Header.second;
		curl_slist* NewHeaderList = curl_slist_append(NewTransfer.HeaderList, HeaderLine.c_str());
		if (NewHeaderList == nullptr)
		{
			Result.ErrorMessage = "Could not add header " + Header.first;
			return false;
		}

		NewTransfer.HeaderList = NewHeaderList;
	}

	if (NewTransfer.HeaderList != nullptr)
		curl_easy_setopt(EasyHandle, CURLOPT_HTTPHEADER, NewTransfer.HeaderList);

	// Method that curl uses without CURLOPT_CUSTOMREQUEST.
	const char* DefaultMethodName = "GET";
	if (!RequestData.FormParts.empty())
	{
		NewTransfer.Form = curl_mime_init(EasyHandle);
		for (const FormPart& Part : RequestData.FormParts)
		{
			curl_mimepart* NewPart = curl_mime_addpart(NewTransfer.Form);
			curl_mime_name(NewPart, Part.Name.c_str());
			if (!Part.FilePath.empty())
			{
				if (curl_mime_filedata(NewPart, Part.FilePath.c_str()) != CURLE_OK)
				{
					Result.ErrorMessage = "Could not read file " + Part.FilePath;
					return false;
				}
			}
			else
			{
				curl_mime_data(NewPart, Part.Value.data(), Part.Value.size());
			}

			if (!Part.ContentType.empty())
				curl_mime_type(NewPart, Part.ContentType.c_str());
		}

		curl_easy_setopt(EasyHandle, CURLOPT_MIMEPOST, NewTransfer.Form);
		DefaultMethodName = "POST";
	}
	else if (!RequestData.Body.empty() || RequestData.Method == HTTPMethod::Post || RequestData.Method == HTTPMethod::Put || RequestData.Method == HTTPMethod::Patch)
	{
		// Body is owned by RequestData, so curl does not need its own copy.
		curl_easy_setopt(EasyHandle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(RequestData.Body.size()));
		curl_easy_setopt(EasyHandle, CURLOPT_POSTFIELDS, RequestData.Body.c_str());
		DefaultMethodName = "POST";
	}

	if (RequestData.Method == HTTPMethod::Head)
	{
		curl_easy_setopt(EasyHandle, CURLOPT_NOBODY, 1L);
	}
	else if (strcmp(GetMethodName(RequestData.Method), DefaultMethodName) != 0)
	{
		curl_easy_setopt(EasyHandle, CURLOPT_CUSTOMREQUEST, GetMethodName(RequestData.Method));
	}

	if (!RequestData.DownloadFilePath.empty())
	{
		// Data goes to temporary file, so existing file is not damaged if transfer fails.
		NewTransfer.TemporaryDownloadFilePath = RequestData.DownloadFilePath + ".download";
		if (_wfopen_s(&NewTransfer.DownloadFileHandle, ToWidePath(NewTransfer.TemporaryDownloadFilePath).c_str(), L"wb") != 0)
		{
			Result.ErrorMessage = "Could not open file " + NewTransfer.TemporaryDownloadFilePath + " for writing";
			return false;
		}

		// Otherwise error page of the server would be saved as the file.
		curl_easy_setopt(EasyHandle, CURLOPT_FAILONERROR, 1L);
	}

	if (RequestData.OnProgress)
	{
		curl_easy_setopt(EasyHandle, CURLOPT_XFERINFOFUNCTION, UpdateTransferProgress);
		curl_easy_setopt(EasyHandle, CURLOPT_XFERINFODATA, &NewTransfer);
		curl_easy_setopt(EasyHandle, CURLOPT_NOPROGRESS, 0L);
	}

	return true;
}

void CurlPlugin::CompleteTransfer(Transfer& FinishedTransfer, CURLcode TransferResult)
{
	Response& Result = FinishedTransfer.Result;
	CURL* EasyHandle = FinishedTransfer.EasyHandle;

	Result.bTransferSucceeded = TransferResult == CURLE_OK;
	if (!Result.bTransferSucceeded)
		Result.ErrorMessage = FinishedTransfer.ErrorBuffer[0] != '\0' ? FinishedTransfer.ErrorBuffer : curl_easy_strerror(TransferResult);

	long StatusCode = 0;
	curl_easy_getinfo(EasyHandle, CURLINFO_RESPONSE_CODE, &StatusCode);
	Result.StatusCode = static_cast<int>(StatusCode);

	char* EffectiveURL = nullptr;
	if (curl_easy_getinfo(EasyHandle, CURLINFO_EFFECTIVE_URL, &EffectiveURL) == CURLE_OK && EffectiveURL != nullptr)
		Result.EffectiveURL = EffectiveURL;

	curl_off_t TotalTimeInMicroseconds = 0;
	if (curl_easy_getinfo(EasyHandle, CURLINFO_TOTAL_TIME_T, &TotalTimeInMicroseconds) == CURLE_OK)
		Result.TotalTimeInSeconds = static_cast<double>(TotalTimeInMicroseconds) / 1000000.0;

	// Request -1 is the last one, after redirects.
	curl_header* PreviousHeader = nullptr;
	curl_header* CurrentHeader = nullptr;
	while ((CurrentHeader = curl_easy_nextheader(EasyHandle, CURLH_HEADER, -1, PreviousHeader)) != nullptr)
	{
		Result.Headers.emplace_back(CurrentHeader->name, CurrentHeader->value);
		PreviousHeader = CurrentHeader;
	}

	if (FinishedTransfer.DownloadFileHandle != nullptr)
	{
		const bool bFileClosed = fclose(FinishedTransfer.DownloadFileHandle) == 0;
		FinishedTransfer.DownloadFileHandle = nullptr;

		const std::wstring TemporaryFilePath = ToWidePath(FinishedTransfer.TemporaryDownloadFilePath);
		if (Result.bTransferSucceeded && !bFileClosed)
		{
			Result.bTransferSucceeded = false;
			Result.ErrorMessage = "Could not write file " + FinishedTransfer.TemporaryDownloadFilePath;
		}
		else if (Result.bTransferSucceeded && !MoveFileExW(TemporaryFilePath.c_str(), ToWidePath(FinishedTransfer.RequestData.DownloadFilePath).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
		{
			Result.bTransferSucceeded = false;
			Result.ErrorMessage = "Could not replace file " + FinishedTransfer.RequestData.DownloadFilePath;
		}

		if (!Result.bTransferSucceeded)
			DeleteFileW(TemporaryFilePath.c_str());
	}

	ReleaseTransferResources(FinishedTransfer);
}

void CurlPlugin::ReleaseTransferResources(Transfer& OldTransfer)
{
	if (OldTransfer.DownloadFileHandle != nullptr)
	{
		fclose(OldTransfer.DownloadFileHandle);
		OldTransfer.DownloadFileHandle = nullptr;
		DeleteFileW(ToWidePath(OldTransfer.TemporaryDownloadFilePath).c_str());
	}

	// Form and header list are used by easy handle, so they are freed after it.
	curl_easy_cleanup(OldTransfer.EasyHandle);
	OldTransfer.EasyHandle = nullptr;
	curl_mime_free(OldTransfer.Form);
	OldTransfer.Form = nullptr;
	curl_slist_free_all(OldTransfer.HeaderList);
	OldTransfer.HeaderList = nullptr;
}

void CurlPlugin::AddFinishedTransfer(const std::shared_ptr<Transfer>& FinishedTransfer)
{
	std::lock_guard<std::mutex> Lock(TransfersMutex);
	FinishedTransfers.push_back(FinishedTransfer);
}

bool CurlPlugin::RemoveActiveTransfer(uint64_t RequestID)
{
	std::lock_guard<std::mutex> Lock(TransfersMutex);
	return ActiveTransfers.erase(RequestID) == 1;
}

void CurlPlugin::RunWorker()
{
	std::unordered_map<uint64_t, std::shared_ptr<Transfer>> RunningTransfers;
	std::vector<std::shared_ptr<Transfer>> NewTransfers;
	std::vector<uint64_t> CancelledRequestIDs;

	while (!bStopWorker)
	{
		{
			std::lock_guard<std::mutex> Lock(TransfersMutex);
			NewTransfers.swap(TransfersToStart);
			CancelledRequestIDs.swap(RequestIDsToCancel);
		}

		for (const std::shared_ptr<Transfer>& NewTransfer : NewTransfers)
		{
			const CURLMcode AddResult = curl_multi_add_handle(MultiHandle, NewTransfer->EasyHandle);
			if (AddResult != CURLM_OK)
			{
				NewTransfer->Result.ErrorMessage = curl_multi_strerror(AddResult);
				ReleaseTransferResources(*NewTransfer);
				AddFinishedTransfer(NewTransfer);
				continue;
			}

			RunningTransfers[NewTransfer->RequestID] = NewTransfer;
		}
		NewTransfers.clear();

		// Requests are cancelled after they are added, because request could be sent and cancelled before worker took it.
		for (const uint64_t RequestID : CancelledRequestIDs)
		{
			auto TransferIterator = RunningTransfers.find(RequestID);
			if (TransferIterator == RunningTransfers.end())
				continue;

			curl_multi_remove_handle(MultiHandle, TransferIterator->second->EasyHandle);
			ReleaseTransferResources(*TransferIterator->second);
			RunningTransfers.erase(TransferIterator);
		}
		CancelledRequestIDs.clear();

		int RunningTransferCount = 0;
		curl_multi_perform(MultiHandle, &RunningTransferCount);

		int MessagesLeft = 0;
		CURLMsg* Message = nullptr;
		while ((Message = curl_multi_info_read(MultiHandle, &MessagesLeft)) != nullptr)
		{
			if (Message->msg != CURLMSG_DONE)
				continue;

			// Message is not valid after handle is removed.
			CURL* EasyHandle = Message->easy_handle;
			const CURLcode TransferResult = Message->data.result;

			char* PrivateData = nullptr;
			curl_easy_getinfo(EasyHandle, CURLINFO_PRIVATE, &PrivateData);
			curl_multi_remove_handle(MultiHandle, EasyHandle);

			auto TransferIterator = RunningTransfers.find(reinterpret_cast<Transfer*>(PrivateData)->RequestID);
			CompleteTransfer(*TransferIterator->second, TransferResult);
			AddFinishedTransfer(TransferIterator->second);
			RunningTransfers.erase(TransferIterator);
		}

		// SendRequest, CancelRequest and OnUnload wake it up with curl_multi_wakeup.
		curl_multi_poll(MultiHandle, nullptr, 0, 1000, nullptr);
	}

	for (const std::pair<const uint64_t, std::shared_ptr<Transfer>>& RunningTransfer : RunningTransfers)
	{
		curl_multi_remove_handle(MultiHandle, RunningTransfer.second->EasyHandle);
		ReleaseTransferResources(*RunningTransfer.second);
	}
}

uint64_t CurlPlugin::SendRequest(const Request& NewRequest, ResponseCallback OnResponse)
{
	std::shared_ptr<Transfer> NewTransfer = std::make_shared<Transfer>();
	NewTransfer->RequestID = NextRequestID++;
	NewTransfer->RequestData = NewRequest;
	NewTransfer->OnResponse = std::move(OnResponse);

	// Failed request is reported through OnResponse like any other, so callers handle errors in one place.
	const bool bPrepared = PrepareTransfer(*NewTransfer);
	if (!bPrepared)
		ReleaseTransferResources(*NewTransfer);

	{
		std::lock_guard<std::mutex> Lock(TransfersMutex);
		ActiveTransfers[NewTransfer->RequestID] = NewTransfer;
		if (bPrepared)
		{
			TransfersToStart.push_back(NewTransfer);
		}
		else
		{
			FinishedTransfers.push_back(NewTransfer);
		}
	}

	if (bPrepared)
		curl_multi_wakeup(MultiHandle);

	return NewTransfer->RequestID;
}

CurlPlugin::Response CurlPlugin::SendRequestBlocking(const Request& NewRequest)
{
	Transfer BlockingTransfer;
	BlockingTransfer.RequestID = NextRequestID++;
	BlockingTransfer.RequestData = NewRequest;
	BlockingTransfer.bBlocking = true;

	if (PrepareTransfer(BlockingTransfer))
	{
		CompleteTransfer(BlockingTransfer, curl_easy_perform(BlockingTransfer.EasyHandle));
	}
	else
	{
		ReleaseTransferResources(BlockingTransfer);
	}

	return std::move(BlockingTransfer.Result);
}

void CurlPlugin::CancelRequest(uint64_t RequestID)
{
	{
		std::lock_guard<std::mutex> Lock(TransfersMutex);
		if (ActiveTransfers.erase(RequestID) == 0)
			return;

		RequestIDsToCancel.push_back(RequestID);
	}

	curl_multi_wakeup(MultiHandle);
}

void CurlPlugin::CancelAllRequests()
{
	{
		std::lock_guard<std::mutex> Lock(TransfersMutex);
		if (ActiveTransfers.empty())
			return;

		for (const std::pair<const uint64_t, std::shared_ptr<Transfer>>& ActiveTransfer : ActiveTransfers)
			RequestIDsToCancel.push_back(ActiveTransfer.first);

		ActiveTransfers.clear();
	}

	curl_multi_wakeup(MultiHandle);
}

bool CurlPlugin::IsRequestActive(uint64_t RequestID) const
{
	std::lock_guard<std::mutex> Lock(TransfersMutex);
	return ActiveTransfers.find(RequestID) != ActiveTransfers.end();
}

size_t CurlPlugin::GetActiveRequestCount() const
{
	std::lock_guard<std::mutex> Lock(TransfersMutex);
	return ActiveTransfers.size();
}

std::string CurlPlugin::EncodeURLComponent(const std::string& Text) const
{
	char* EncodedText = curl_easy_escape(nullptr, Text.c_str(), static_cast<int>(Text.size()));
	if (EncodedText == nullptr)
		return "";

	std::string Result = EncodedText;
	curl_free(EncodedText);
	return Result;
}

std::string CurlPlugin::DecodeURLComponent(const std::string& Text) const
{
	int DecodedLength = 0;
	char* DecodedText = curl_easy_unescape(nullptr, Text.c_str(), static_cast<int>(Text.size()), &DecodedLength);
	if (DecodedText == nullptr)
		return "";

	std::string Result(DecodedText, static_cast<size_t>(DecodedLength));
	curl_free(DecodedText);
	return Result;
}

std::string CurlPlugin::GetCurlVersion() const
{
	const curl_version_info_data* VersionData = curl_version_info(CURLVERSION_NOW);
	// Version text of curl built from git tag has "-DEV" suffix, only release archives replace it, so number is used.
	const unsigned int VersionNumber = VersionData->version_num;
	std::string Result = std::to_string((VersionNumber >> 16) & 0xFF) + "." + std::to_string((VersionNumber >> 8) & 0xFF) + "." + std::to_string(VersionNumber & 0xFF);
	if (VersionData->ssl_version != nullptr)
		Result += ", " + std::string(VersionData->ssl_version);

	return Result;
}