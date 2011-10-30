/* This file is part of pr-downloader (GPL v2 or later), see the LICENSE file */

#include "Sdp.h"
#include "RapidDownloader.h"
#include "RepoMaster.h"
#include "Util.h"
#include "Logger.h"
#include "FileSystem/FileSystem.h"
#include "FileSystem/FileData.h"
#include "FileSystem/HashMD5.h"

#include <string>
#include <string.h>
#include <stdio.h>
#include <curl/curl.h>
#include <stdlib.h>

CSdp::CSdp(const std::string& shortname, const std::string& md5, const std::string& name, const std::string& depends, const std::string& url)
{
	this->shortname=shortname;
	this->name=name;
	this->md5=md5;
	this->url=url;
	this->downloaded=false;
	this->depends=depends;
	this->file_handle=NULL;
	this->downlooadInitialized=false;
	this->cursize=0;
	memset(this->cursize_buf,LENGTH_SIZE,0);
	this->skipped=false;
	this->file_pos=0;
}

bool CSdp::download()
{
	if (downloaded) //allow download only once of the same sdp
		return true;
	filename=fileSystem->getSpringDir() + PATH_DELIMITER+"packages"+PATH_DELIMITER;
	LOG_DEBUG("%s",filename.c_str());
	if (!fileSystem->directoryExists(filename)) {
		fileSystem->createSubdirs(filename);
	}
	int count=0;
	filename  += this->md5 + ".sdp";
	FileData tmp=FileData();
	HashMD5 md5= HashMD5();
	md5.Set(tmp.md5, sizeof(tmp.md5));
	std::list<FileData> files;

	if (!fileSystem->parseSdp(filename,files)) { //file isn't avaiable, download it
		IDownload dl(filename);
		dl.addMirror(url + "/packages/" + md5.toString() + ".sdp");
		httpDownload->download(&dl);
		fileSystem->parseSdp(filename,files); //parse downloaded file
	}

	std::list<FileData>::iterator it;
	/*	CHttpDownload* tmp=httpDownload; //FIXME: extend interface?
		tmp->setCount(files.size());
	*/
	int i=0;
	it=files.begin();
	while (it!=files.end()) {
		i++;
		md5.Set((*it).md5, sizeof((*it).md5));
		std::string md5str=md5.toString();
		std::string filename=md5str.substr(2);
		filename.append(".gz");
		std::string path("/pool/");
		path += md5str.at(0);
		path += md5str.at(1);
		path += "/";

		std::string file=fileSystem->getSpringDir() + path + filename; //absolute filename

		if (!fileSystem->directoryExists(fileSystem->getSpringDir()+path)) {
			fileSystem->createSubdirs(fileSystem->getSpringDir()+path);
		}

		if (!fileSystem->fileExists(file)) { //add non-existing files to download list
			count++;
			(*it).download=true;
		} else {
			(*it).download=false;
		}
		if (i%10==0) {
			LOG_DEBUG("\r%d/%d checked",i,(int)files.size());
		}
		++it;
	}
	LOG_DEBUG("\r%d/%d need to download %d files",i,(unsigned int)files.size(),count);
	if (count>0) {
//FIXME	httpDownload->setCount(count);
		downloaded=downloadStream(this->url+"/streamer.cgi?"+this->md5,files);
		files.clear();
		LOG_DEBUG("Sucessfully downloaded %d files: %s %s",count,shortname.c_str(),name.c_str());
	} else {
		LOG_DEBUG("Already downloaded: %s", shortname.c_str());
		downloaded=true;
	}
	return downloaded;
}

/**
	write the data received from curl to the rapid pool.

	the filename is read from the sdp-list (created at request start)
	filesize is read from the http-data received (could overlap!)
*/
static size_t write_streamed_data(const void* tmp, size_t size, size_t nmemb,CSdp *sdp)
{
	char buf[CURL_MAX_WRITE_SIZE];
	memcpy(&buf,tmp,CURL_MAX_WRITE_SIZE);
	if (!sdp->downlooadInitialized) {
		sdp->list_it=sdp->globalFiles->begin();
		sdp->downlooadInitialized=true;
		sdp->file_handle=NULL;
		sdp->file_name="";
		sdp->skipped=0;
	}
	char* buf_start=(char*)&buf;
	const char* buf_end=buf_start + size*nmemb;
	char* buf_pos=buf_start;

	while (buf_pos<buf_end) { //all bytes written?
		if (sdp->file_handle==NULL) { //no open file, create one
			while ( (!(*sdp->list_it).download==true) && (sdp->list_it!=sdp->globalFiles->end())) { //get file
				sdp->list_it++;
			}
			sdp->file_name=fileSystem->getPoolFileName((*sdp->list_it).name.c_str());
			sdp->file_handle=fopen(sdp->file_name.c_str(),"wb");
//FIXME		sdp->setStatsPos(sdp->getStatsPos()+1);
			if (sdp->file_handle==NULL) {
				LOG_ERROR("couldn't open %s",(*sdp->list_it).name.c_str());
				return -1;
			}
			//here comes the init new file stuff
			sdp->file_pos=0;
		}
		if (sdp->file_handle!=NULL) {
			if ((sdp->skipped>0)&&(sdp->skipped<4)) {
				LOG_DEBUG("difficulty %d",sdp->skipped);
			}
			if (sdp->skipped<4) { // check if we skipped all 4 bytes, if not so, skip them
				int toskip=intmin(buf_end-buf_pos,LENGTH_SIZE-sdp->skipped); //calculate bytes we can skip, could overlap received bufs
				for (int i=0; i<toskip; i++) //copy bufs avaiable
					sdp->cursize_buf[i]=buf_pos[i];
				LOG_DEBUG("toskip: %d skipped: %d",toskip,sdp->skipped);
				sdp->skipped=toskip+sdp->skipped;
				buf_pos=buf_pos+sdp->skipped;
				if (sdp->skipped==LENGTH_SIZE) {
					(*sdp->list_it).compsize=parse_int32(sdp->cursize_buf);
				}
			}
			if (sdp->skipped==LENGTH_SIZE) {
				int towrite=intmin ((*sdp->list_it).compsize-sdp->file_pos ,  //minimum of bytes to write left in file and bytes to write left in buf
						    buf_end-buf_pos);
//				LOG_DEBUG("%s %d %ld %ld %ld %d %d %d %d %d",sdp->file_name.c_str(), (*sdp->list_it).compsize, buf_pos,buf_end, buf_start, towrite, size, nmemb , sdp->skipped, sdp->file_pos);
				int res=0;
				if (towrite>0) {
					res=fwrite(buf_pos,1,towrite,sdp->file_handle);
					if (res!=towrite) {
						LOG_ERROR("fwrite error");
						return -1;
					}
					if (res<=0) {
						LOG_ERROR("\nwrote error: %d", res);
						return -1;
					}
				} else if (towrite<0) {
					LOG_DEBUG("%s","Fatal, something went wrong here!");
					return -1;
				}

				buf_pos=buf_pos+res;
				sdp->file_pos+=res;
				if (sdp->file_pos>=(*sdp->list_it).compsize) { //file finished -> next file
					fclose(sdp->file_handle);
					if (!fileSystem->fileIsValid(*sdp->list_it,sdp->file_name.c_str())) {
						LOG_ERROR("File is broken?!: %s",sdp->file_name.c_str());
						return -1;
					}
					sdp->file_handle=NULL;
					sdp->list_it++;
					sdp->file_pos=0;
					sdp->skipped=0;
				}
			}
		}
	}
	return buf_pos-buf_start;

}

/** *
	draw a nice download status-bar
*/
static int progress_func(CSdp& csdp, double TotalToDownload, double NowDownloaded,
			 double TotalToUpload, double NowUploaded)
{

	(void)csdp;
	(void)TotalToUpload;
	(void)NowUploaded; //remove unused warning

	LOG_PROGRESS(NowDownloaded,TotalToDownload);
	return 0;
}

bool CSdp::downloadStream(std::string url,std::list<FileData>& files)
{
	CURL* curl;
	curl = curl_easy_init();
	if (curl) {
		CURLcode res;
		LOG_INFO("Using rapid");
		LOG_DOWNLOAD(url.c_str());

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

		std::list<FileData>::iterator it;
		int  buflen=files.size()/8;
		if (files.size()%8!=0)
			buflen++;
		char* buf=(char*)malloc(buflen); //FIXME: compress blockwise and not all at once
		memset(buf,0,buflen);
		int destlen=files.size()*2;
		LOG_DEBUG("%d %d %d",(int)files.size(),buflen,destlen);
		int i=0;
		for (it=files.begin(); it!=files.end(); ++it) {
			if ((*it).download==true)
				buf[i/8] = buf[i/8] + (1<<(i%8));
			i++;
		}
		char* dest=(char*)malloc(destlen);

		gzip_str(buf,buflen,dest,&destlen);

		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_streamed_data);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);

		globalFiles=&files;
		curl_easy_setopt(curl, CURLOPT_FAILONERROR, true);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, dest);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,destlen);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progress_func);

		res = curl_easy_perform(curl);
		free(dest);
		/* always cleanup */
		curl_easy_cleanup(curl);
		if (res!=CURLE_OK) {
			LOG_ERROR("%s",curl_easy_strerror(res));
			return false;
		}
	}
	return true;
}
