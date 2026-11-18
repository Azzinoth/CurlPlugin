// curl.h includes winsock2.h, it should be before windows.h from engine headers, otherwise windows.h includes winsock.h which conflicts with it.
#include <curl/curl.h>
#include "SubSystems/FocalEngine/SubSystems/PluginSystem/FEPluginConnector.h"
#include "Public/CurlPluginInterface.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
using namespace FocalEngine;

class CurlPlugin : public FEPluginCore, public CurlPluginInterface
{
	struct Transfer
	{
		uint64_t RequestID = 0;
		Request RequestData;
		ResponseCallback OnResponse;
		bool bBlocking = false;

		CURL* EasyHandle = nullptr;
		curl_slist* HeaderList = nullptr;
		curl_mime* Form = nullptr;
		FILE* DownloadFileHandle = nullptr;
		std::string TemporaryDownloadFilePath;
		char ErrorBuffer[CURL_ERROR_SIZE] = {};

		// Written by plugin thread, read by main thread.
		std::atomic<uint64_t> DownloadedBytes{ 0 };
		std::atomic<uint64_t> TotalBytesToDownload{ 0 };
		std::atomic<uint64_t> UploadedBytes{ 0 };
		std::atomic<uint64_t> TotalBytesToUpload{ 0 };
		std::atomic<bool> bProgressChanged{ false };

		Response Result;
	};

	CURLM* MultiHandle = nullptr;
	std::thread WorkerThread;
	std::atomic<bool> bStopWorker{ false };
	std::atomic<uint64_t> NextRequestID{ 1 };

	mutable std::mutex TransfersMutex;
	// Requests whose OnResponse was not called yet, callbacks of requests that are not here are never called.
	std::unordered_map<uint64_t, std::shared_ptr<Transfer>> ActiveTransfers;
	std::vector<std::shared_ptr<Transfer>> TransfersToStart;
	std::vector<uint64_t> RequestIDsToCancel;
	std::vector<std::shared_ptr<Transfer>> FinishedTransfers;

	static size_t WriteResponseData(char* Data, size_t Size, size_t Count, void* UserData);
	static int UpdateTransferProgress(void* UserData, curl_off_t TotalBytesToDownload, curl_off_t DownloadedBytes, curl_off_t TotalBytesToUpload, curl_off_t UploadedBytes);

	// On failure Result.ErrorMessage is set, ReleaseTransferResources should be called in any case.
	bool PrepareTransfer(Transfer& NewTransfer);
	void CompleteTransfer(Transfer& FinishedTransfer, CURLcode TransferResult);
	void ReleaseTransferResources(Transfer& OldTransfer);
	void AddFinishedTransfer(const std::shared_ptr<Transfer>& FinishedTransfer);
	// Returns false if request was cancelled.
	bool RemoveActiveTransfer(uint64_t RequestID);
	void ReportProgress(Transfer& CurrentTransfer);
	void RunWorker();
public:
	bool OnLoad() override;
	void OnUnload() override;
	void OnUpdate(double DeltaTime) override;

	uint64_t SendRequest(const Request& NewRequest, ResponseCallback OnResponse) override;
	Response SendRequestBlocking(const Request& NewRequest) override;
	void CancelRequest(uint64_t RequestID) override;
	void CancelAllRequests() override;
	bool IsRequestActive(uint64_t RequestID) const override;
	size_t GetActiveRequestCount() const override;
	std::string EncodeURLComponent(const std::string& Text) const override;
	std::string DecodeURLComponent(const std::string& Text) const override;
	std::string GetCurlVersion() const override;
};