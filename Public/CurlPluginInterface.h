#pragma once
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Public API of CurlPlugin, applications include only this header.
// Usage:
//	using PluginInterface = CurlPluginInterface;
//	PluginInterface* Plugin = PLUGIN_MANAGER.GetPlugin<PluginInterface>(UNIQUE_ID.FromString(PluginInterface::PluginID));
//	if (Plugin != nullptr && Plugin->GetInterfaceVersion() == PluginInterface::InterfaceVersion)
//		Plugin->...;
class CurlPluginInterface
{
public:
	// Same as "ID" in CurlPlugin.json.
	static constexpr const char* PluginID = "e70bc211-9ca4-4884-915b-00e455f437d1";

	// Increase when anything in this file changes.
	static constexpr int InterfaceVersion = 1;

	enum class HTTPMethod
	{
		Get,
		Post,
		Put,
		Patch,
		Delete,
		Head
	};

	// One part of multipart/form-data body.
	struct FormPart
	{
		std::string Name;
		// Sent when FilePath is empty.
		std::string Value;
		// Content of this file is sent together with its file name.
		std::string FilePath;
		// If empty, curl chooses it by file extension, for Value it is not sent.
		std::string ContentType;
	};

	// Totals are 0 while they are not known.
	using ProgressCallback = std::function<void(uint64_t DownloadedBytes, uint64_t TotalBytesToDownload, uint64_t UploadedBytes, uint64_t TotalBytesToUpload)>;

	struct Request
	{
		// Besides http:// and https://, other URLs that curl supports work too, for example ftp:// or file://.
		std::string URL;
		HTTPMethod Method = HTTPMethod::Get;
		// Header with empty value removes header that curl would add, for example { "Expect", "" }.
		std::vector<std::pair<std::string, std::string>> Headers;
		// Without "Content-Type" header body is sent as application/x-www-form-urlencoded.
		std::string Body;
		// If not empty, multipart/form-data body is built from these parts and Body is ignored.
		std::vector<FormPart> FormParts;
		// If not empty, response body is saved to this file instead of Response::Body.
		// File is replaced only if transfer succeeded and server did not answer with HTTP error, so error page is never saved.
		std::string DownloadFilePath;

		// Basic authentication.
		std::string UserName;
		std::string Password;
		// If empty, User-Agent header is not sent, some servers (for example GitHub API) reject such requests.
		std::string UserAgent;
		// For example "http://proxy.example.com:8080". If empty, http_proxy, https_proxy and no_proxy environment variables are used, Windows proxy settings are not.
		std::string Proxy;

		double ConnectTimeoutInSeconds = 30.0;
		// Limit for the whole transfer, 0 means no limit.
		double TimeoutInSeconds = 0.0;
		bool bFollowRedirects = true;
		// Should be false only for testing with servers that have self-signed certificates.
		bool bVerifyCertificate = true;

		// With SendRequest it is called on the main thread, at most once per frame, with SendRequestBlocking on the calling thread.
		ProgressCallback OnProgress;
	};

	struct Response
	{
		uint64_t RequestID = 0;
		// False if request could not be sent or transfer did not complete.
		// HTTP errors like 404 are successful transfers, StatusCode tells about them.
		bool bTransferSucceeded = false;
		std::string ErrorMessage;
		// HTTP status code, or reply code of other protocols, 0 if there was no response.
		int StatusCode = 0;
		// Headers of the last response, if redirects were followed.
		std::vector<std::pair<std::string, std::string>> Headers;
		// Empty if Request::DownloadFilePath was used.
		std::string Body;
		// URL after redirects.
		std::string EffectiveURL;
		double TotalTimeInSeconds = 0.0;

		bool IsSuccessful() const
		{
			return bTransferSucceeded && StatusCode >= 200 && StatusCode < 300;
		}

		// Name is not case sensitive, returns empty string if there is no such header.
		std::string GetHeaderValue(const std::string& Name) const
		{
			for (const std::pair<std::string, std::string>& Header : Headers)
			{
				if (Header.first.size() != Name.size())
					continue;

				bool bSameName = true;
				for (size_t i = 0; i < Name.size(); i++)
				{
					if (std::tolower(static_cast<unsigned char>(Header.first[i])) != std::tolower(static_cast<unsigned char>(Name[i])))
					{
						bSameName = false;
						break;
					}
				}

				if (bSameName)
					return Header.second;
			}

			return "";
		}
	};

	using ResponseCallback = std::function<void(const Response& Result)>;

	// Order of virtual functions is part of the interface, new functions should be added only at the end.
	virtual ~CurlPluginInterface() = default;
	virtual int GetInterfaceVersion() const { return InterfaceVersion; }

	// Request is performed on plugin thread, OnResponse is called on the main thread during frame update, also when request fails.
	// Can be called from any thread, returns ID of request.
	virtual uint64_t SendRequest(const Request& NewRequest, ResponseCallback OnResponse) = 0;
	// Blocks calling thread until request is finished, can be called from any thread.
	// Useful in applications without frame loop, where callbacks of SendRequest are never called.
	virtual Response SendRequestBlocking(const Request& NewRequest) = 0;

	// After it returns OnResponse and OnProgress of that request are not called, so objects they capture can be deleted.
	virtual void CancelRequest(uint64_t RequestID) = 0;
	virtual void CancelAllRequests() = 0;
	// True until OnResponse of request is called or request is cancelled.
	virtual bool IsRequestActive(uint64_t RequestID) const = 0;
	virtual size_t GetActiveRequestCount() const = 0;

	// Percent-encodes everything except letters, digits and "-._~", for example to put text in URL query.
	virtual std::string EncodeURLComponent(const std::string& Text) const = 0;
	// "+" is not turned into space.
	virtual std::string DecodeURLComponent(const std::string& Text) const = 0;

	// For example "8.23.0, Schannel".
	virtual std::string GetCurlVersion() const = 0;

	uint64_t Get(const std::string& URL, ResponseCallback OnResponse)
	{
		Request NewRequest;
		NewRequest.URL = URL;
		return SendRequest(NewRequest, std::move(OnResponse));
	}

	uint64_t PostJSON(const std::string& URL, const std::string& JSON, ResponseCallback OnResponse)
	{
		Request NewRequest;
		NewRequest.URL = URL;
		NewRequest.Method = HTTPMethod::Post;
		NewRequest.Headers.push_back({ "Content-Type", "application/json" });
		NewRequest.Body = JSON;
		return SendRequest(NewRequest, std::move(OnResponse));
	}

	uint64_t DownloadFile(const std::string& URL, const std::string& FilePath, ResponseCallback OnResponse, ProgressCallback OnProgress = nullptr)
	{
		Request NewRequest;
		NewRequest.URL = URL;
		NewRequest.DownloadFilePath = FilePath;
		NewRequest.OnProgress = std::move(OnProgress);
		return SendRequest(NewRequest, std::move(OnResponse));
	}
};