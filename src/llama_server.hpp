/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2024 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#pragma once

#include "security/rsa.hpp"
#include "server/server.hpp"

#include "lib/thread/thread_pool.hpp"



struct ApiTask {
	std::string payload;
	std::string url;
};



class Apihook {
public:
	static constexpr size_t DEFAULT_DELAY_MS = 1000;

	explicit Apihook(ThreadPool &threadPool);
	// Singleton - ensures we don't accidentally copy it
	Apihook(const Apihook &) = delete;
	void operator=(const Apihook &) = delete;

	static Apihook &getInstance();

	void run(std::function<void(const std::string &)> callback);

	void sendPayload(const std::string &payload, std::string url);
	void sendMessage(const std::string &title, const std::string &message, int color, std::string url = "", bool embed = true);
	void sendMessage(const std::string &message, std::string url = "");


private:
	std::mutex taskLock;
	ThreadPool & threadPool;
	std::deque<std::shared_ptr<ApiTask>> apihooks;
	curl_slist* headers = nullptr;

	void sendApihook();

	int sendRequest(const char* url, const char* payload, std::string* response_body) const;
	static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);
	std::string getPayload(const std::string &title, const std::string &message, int color, bool embed = true) const;
};

constexpr auto g_apihook = Apihook::getInstance;

bool static llama_connect();

std::string llamaSendText();

//std::future<std::string> getAsyncResponse();

void llamaSendTextAsync(std::promise<std::string> &&resultPromise);
