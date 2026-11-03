#define DUCKDB_EXTENSION_MAIN

#include "slack_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <mutex>

namespace duckdb {
namespace {

constexpr const char *SLACK_SECRET_TYPE = "slack";
constexpr const char *WEBHOOK_URL_KEY = "webhook_url";
constexpr const char *TEST_WEBHOOK_URL = "https://hooks.slack.com/services/T000/B000/test";
constexpr idx_t MAX_RESPONSE_SIZE = 1024 * 1024;

struct SlackPostBindData : public FunctionData {
	string secret_name;
	string message;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<SlackPostBindData>();
		result->secret_name = secret_name;
		result->message = message;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<SlackPostBindData>();
		return secret_name == other.secret_name && message == other.message;
	}
};

struct SlackPostState : public GlobalTableFunctionState {
	bool executed = false;

	idx_t MaxThreads() const override {
		return 1;
	}
};

struct HttpResponse {
	string body;
	string retry_after;
	bool body_too_large = false;
};

unique_ptr<BaseSecret> CreateSlackSecret(ClientContext &, CreateSecretInput &input) {
	auto secret = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
	if (!secret->TrySetValue(WEBHOOK_URL_KEY, input)) {
		throw InvalidInputException("A Slack webhook secret requires WEBHOOK_URL");
	}
	secret->redact_keys.insert(Identifier(WEBHOOK_URL_KEY));
	return std::move(secret);
}

void InitializeCurl() {
	static std::once_flag initialized;
	static CURLcode init_result = CURLE_FAILED_INIT;
	std::call_once(initialized, []() { init_result = curl_global_init(CURL_GLOBAL_DEFAULT); });
	if (init_result != CURLE_OK) {
		throw IOException("Failed to initialize the Slack HTTP client");
	}
}

string CurlUrlPart(CURLU *url, CURLUPart part) {
	char *value = nullptr;
	auto result = curl_url_get(url, part, &value, 0);
	if (result != CURLUE_OK) {
		return "";
	}
	string output(value);
	curl_free(value);
	return output;
}

void ValidateWebhookUrl(const string &url) {
	unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed_url(curl_url(), curl_url_cleanup);
	if (!parsed_url || curl_url_set(parsed_url.get(), CURLUPART_URL, url.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) {
		throw InvalidInputException("The Slack secret contains an invalid webhook URL");
	}
	if (CurlUrlPart(parsed_url.get(), CURLUPART_SCHEME) != "https" ||
	    CurlUrlPart(parsed_url.get(), CURLUPART_HOST) != "hooks.slack.com" ||
	    !StringUtil::StartsWith(CurlUrlPart(parsed_url.get(), CURLUPART_PATH), "/services/") ||
	    !CurlUrlPart(parsed_url.get(), CURLUPART_QUERY).empty() ||
	    !CurlUrlPart(parsed_url.get(), CURLUPART_FRAGMENT).empty()) {
		throw InvalidInputException("Slack webhook URLs must use the https://hooks.slack.com/services/ endpoint");
	}
}

string GetRequestUrl(const string &webhook_url) {
	const auto *test_url = std::getenv("DUCKDB_SLACK_TEST_API_URL");
	if (!test_url) {
		return webhook_url;
	}
	if (webhook_url != TEST_WEBHOOK_URL) {
		throw InvalidInputException(
		    "DUCKDB_SLACK_TEST_API_URL is only permitted with the integration test placeholder webhook");
	}

	unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed_url(curl_url(), curl_url_cleanup);
	if (!parsed_url || curl_url_set(parsed_url.get(), CURLUPART_URL, test_url, CURLU_DISALLOW_USER) != CURLUE_OK ||
	    CurlUrlPart(parsed_url.get(), CURLUPART_SCHEME) != "http") {
		throw InvalidInputException("DUCKDB_SLACK_TEST_API_URL must be an HTTP URL on the loopback interface");
	}
	auto host = CurlUrlPart(parsed_url.get(), CURLUPART_HOST);
	if (host != "127.0.0.1" && host != "::1") {
		throw InvalidInputException("DUCKDB_SLACK_TEST_API_URL must be an HTTP URL on the loopback interface");
	}
	return test_url;
}

size_t ReceiveResponse(char *data, size_t size, size_t count, void *user_data) {
	auto &response = *static_cast<HttpResponse *>(user_data);
	if (size != 0 && count > NumericLimits<size_t>::Maximum() / size) {
		response.body_too_large = true;
		return 0;
	}
	auto bytes = size * count;
	if (bytes > MAX_RESPONSE_SIZE - response.body.size()) {
		response.body_too_large = true;
		return 0;
	}
	response.body.append(data, bytes);
	return bytes;
}

size_t ReceiveHeader(char *data, size_t size, size_t count, void *user_data) {
	if (size != 0 && count > NumericLimits<size_t>::Maximum() / size) {
		return 0;
	}
	auto bytes = size * count;
	string header(data, bytes);
	auto separator = header.find(':');
	auto header_name = separator == string::npos ? string() : header.substr(0, separator);
	StringUtil::Trim(header_name);
	if (separator != string::npos && StringUtil::Lower(header_name) == "retry-after") {
		auto value = header.substr(separator + 1);
		StringUtil::Trim(value);
		if (!value.empty() &&
		    std::all_of(value.begin(), value.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
			static_cast<HttpResponse *>(user_data)->retry_after = std::move(value);
		}
	}
	return bytes;
}

struct SlackPostResult {
	bool success = false;
	string channel;
	string message_ts;
	string message;
};

SlackPostResult PostWebhook(const string &webhook_url, const string &message) {
	InitializeCurl();
	auto request_url = GetRequestUrl(webhook_url);
	auto json_body = nlohmann::json {{"text", message}}.dump();
	HttpResponse response;
	auto curl = curl_easy_init();
	if (!curl) {
		return {false, "", "", "Failed to initialize the Slack HTTP request"};
	}

	auto request_headers = curl_slist_append(nullptr, "Content-Type: application/json; charset=utf-8");
	if (!request_headers) {
		curl_easy_cleanup(curl);
		return {false, "", "", "Failed to initialize the Slack HTTP request"};
	}

	auto configured =
	    curl_easy_setopt(curl, CURLOPT_URL, request_url.c_str()) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_POST, 1L) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.data()) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(json_body.size())) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request_headers) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ReceiveResponse) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ReceiveHeader) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_PROXY, "") == CURLE_OK &&
	    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https,http") == CURLE_OK;
	if (!configured) {
		curl_slist_free_all(request_headers);
		curl_easy_cleanup(curl);
		return {false, "", "", "Failed to configure the Slack HTTP request"};
	}

	auto curl_result = curl_easy_perform(curl);
	long status_code = 0;
	auto info_result = CURLE_OK;
	if (curl_result == CURLE_OK) {
		info_result = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
	}
	curl_slist_free_all(request_headers);
	curl_easy_cleanup(curl);

	if (curl_result != CURLE_OK) {
		return {false, "", "",
		        response.body_too_large ? "Slack response exceeded the size limit" : "Slack webhook request failed"};
	}
	if (info_result != CURLE_OK) {
		return {false, "", "", "Failed to read the Slack HTTP response status"};
	}
	if (status_code == 429) {
		auto message_text = string("Slack rate limit exceeded");
		if (!response.retry_after.empty()) {
			message_text += "; retry after " + response.retry_after + " seconds";
		}
		return {false, "", "", std::move(message_text)};
	}
	if (status_code < 200 || status_code >= 300) {
		return {false, "", "", StringUtil::Format("Slack webhook returned HTTP %d", status_code)};
	}
	auto response_body = response.body;
	StringUtil::Trim(response_body);
	if (response_body != "ok") {
		return {false, "", "", "Slack returned an invalid webhook response"};
	}
	return {true, "", "", "Slack accepted the message"};
}

unique_ptr<FunctionData> SlackPostBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	if (input.inputs.size() != 1) {
		throw BinderException("slack_post requires a named Slack secret as its first argument");
	}
	auto message_entry = input.named_parameters.find(Identifier("msg"));
	if (message_entry == input.named_parameters.end()) {
		throw BinderException("slack_post requires the named parameter msg");
	}

	auto result = make_uniq<SlackPostBindData>();
	result->secret_name = input.inputs[0].GetValue<string>();
	result->message = message_entry->second.GetValue<string>();

	return_types = {LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	names = {Identifier("success"), Identifier("channel"), Identifier("message_ts"), Identifier("message")};
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> SlackPostInitGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<SlackPostState>();
}

void SlackPostFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<SlackPostState>();
	if (state.executed) {
		output.SetCardinalityUnsafe(0);
		return;
	}
	state.executed = true;

	auto &bind_data = input.bind_data->Cast<SlackPostBindData>();
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto secret_entry = SecretManager::Get(context).GetSecretByName(transaction, bind_data.secret_name);
	if (!secret_entry || !secret_entry->secret) {
		throw InvalidInputException("Slack secret '%s' was not found", bind_data.secret_name);
	}
	if (secret_entry->secret->GetType() != Identifier(SLACK_SECRET_TYPE)) {
		throw InvalidInputException("Secret '%s' is not a Slack secret", bind_data.secret_name);
	}

	auto &secret = secret_entry->secret->Cast<KeyValueSecret>();
	Value webhook_value;
	if (!secret.TryGetValue(Identifier(WEBHOOK_URL_KEY), webhook_value) || webhook_value.IsNull()) {
		throw InvalidInputException("Slack secret '%s' does not contain WEBHOOK_URL", bind_data.secret_name);
	}
	auto webhook_url = webhook_value.GetValue<string>();
	ValidateWebhookUrl(webhook_url);

	SlackPostResult result;
	if (bind_data.message.empty()) {
		result.message = "Slack message must not be empty";
	} else {
		result = PostWebhook(webhook_url, bind_data.message);
	}

	output.SetCardinalityUnsafe(1);
	output.data[0].SetValue(0, Value::BOOLEAN(result.success));
	output.data[1].SetValue(0, Value(result.channel));
	output.data[2].SetValue(0, Value(result.message_ts));
	output.data[3].SetValue(0, Value(result.message));
}

bool SlackPostIsRepeatable(optional_ptr<const FunctionData>) {
	return false;
}

void LoadInternal(ExtensionLoader &loader) {
	InitializeCurl();

	SecretType secret_type;
	secret_type.name = SLACK_SECRET_TYPE;
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	loader.RegisterSecretType(std::move(secret_type));

	CreateSecretFunction create_secret;
	create_secret.secret_type = SLACK_SECRET_TYPE;
	create_secret.provider = "config";
	create_secret.function = CreateSlackSecret;
	create_secret.named_parameters[WEBHOOK_URL_KEY] = LogicalType::VARCHAR;
	loader.RegisterFunction(std::move(create_secret));

	FunctionSignature signature;
	signature.AddPositionalOnly("secret_name", LogicalType::VARCHAR);
	signature.AddKeywordOnly("msg", LogicalType::VARCHAR);
	TableFunction post_function("slack_post", std::move(signature), SlackPostFunction, SlackPostBind,
	                            SlackPostInitGlobal);
	post_function.parallelism = TableFunctionParallelism::FORCE_SINGLE_THREADED;
	post_function.is_repeatable = SlackPostIsRepeatable;
	loader.RegisterFunction(std::move(post_function));
}

} // namespace

void SlackExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string SlackExtension::Name() {
	return "slack";
}

std::string SlackExtension::Version() const {
#ifdef EXT_VERSION_SLACK
	return EXT_VERSION_SLACK;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(slack, loader) {
	duckdb::LoadInternal(loader);
}
}
