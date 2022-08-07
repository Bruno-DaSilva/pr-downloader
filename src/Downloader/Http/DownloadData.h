/* This file is part of pr-downloader (GPL v2 or later), see the LICENSE file */

#ifndef _DOWNLOAD_DATA_H
#define _DOWNLOAD_DATA_H

#include <memory>
#include <vector>
#include <string>
#include <chrono>

class Mirror;
class IDownload;
class CurlWrapper;

// Used for computing progress across multiple DownloadData downloaded in
// parallel.
class DownloadDataPack
{
public:
	int size = 0;
	int progress = 0;
};

class DownloadData
{
public:
	DownloadData();
	std::unique_ptr<CurlWrapper> curlw; // curl_easy_handle
	std::string mirror;     // mirror used
	IDownload* download;
	DownloadDataPack* data_pack = nullptr;
	int approx_size = 0; // Either approx or real size from the IDownload.
	int retry_num = 0;
	std::chrono::seconds retry_after_from_server{0};
	std::chrono::steady_clock::time_point next_retry;
	bool force_discard = false;

	void updateProgress(double total, double done);
};

#endif
