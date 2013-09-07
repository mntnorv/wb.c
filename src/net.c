/*
 * wb - A wallbase.cc image downloader
 *
 * Copyright (C) 2013 Mantas Norvaiša
 *
 * This file is part of wb.
 * 
 * wb is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * wb is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with wb.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <libgen.h>
#include <math.h>
#include <curl/curl.h>

#include "types.h"
#include "error.h"
#include "net.h"

/* Global CURL handle for reuse */
static CURL *curl_handle = NULL;

/**
 * Initialize the wb net system.
 */
void net_init() {
	curl_global_init(CURL_GLOBAL_ALL);
}

/**
 * Cleanup the wb net system.
 */
void net_cleanup() {
	if (curl_handle != NULL) {
		curl_easy_cleanup(curl_handle);
		curl_handle = NULL;
	}

	curl_global_cleanup();
}

/**
 * Setup the CURL handle. Creates a new handle if it has not already
 * been created, cleans up the handle otherwise.
 */
void
setup_curl_handle() {
	if (curl_handle == NULL) {
		curl_handle = curl_easy_init();
		curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl_handle, CURLOPT_COOKIEFILE, ""); /* Enable the cookie engine */
	} else {
		curl_easy_setopt(curl_handle, CURLOPT_COOKIELIST, "ALL"); /* remove all cookies */
		curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, NULL);
		curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1L);
	}
}

/**
 * Writes CURL response to a curl_response structure.
 *
 * @param ptr - CURL response data pointer.
 * @param size - size of the data to write in units of nmemb.
 * @param nmemb - multiplier of size.
 * @param data - the structure to write data to.
 * @return the size of data written in bytes.
 */
size_t
write_data_to_response(void *ptr, size_t size, size_t nmemb, struct curl_response *data) {
	size_t index = data->size;
	size_t n = (size * nmemb);
	char* tmp;

	data->size += (size * nmemb);

	tmp = realloc(data->data, data->size + 1); /* +1 for '\0' */

	if(tmp) {
		data->data = tmp;
	} else {
		if(data->data) {
			free(data->data);
		}
		wb_error("failed to allocate memory");
		return 0;
	}

	memcpy((data->data + index), ptr, n);
	data->data[data->size] = '\0';

	return size * nmemb;
}

/**
 * Writes CURL response to a FILE.
 *
 * @param ptr - CURL response data pointer.
 * @param size - size of the data to write in units of nmemb.
 * @param nmemb - multiplier of size.
 * @param file - the file to write data to.
 * @return the size of data written in bytes.
 */
size_t
write_data_to_file(void *ptr, size_t size, size_t nmemb, FILE *file) {
	size_t written;
	written = fwrite(ptr, size, nmemb, file);
	return written;
}

/**
 * Reports the current CURL download progress.
 *
 * @param ptr - pointer to the progress data passed to CURL by
 *   the client
 * @param total_down - total number of bytes to download
 * @param now_down - number of bytes downloaded
 * @param total_up - total number of bytes downloaded
 * @param now_up - number of bytes uploaded
 * @return 0 if the download or upload should be continued, any
 *   other number to cancel the operation.
 */
int
download_progress(void* ptr, double total_down, double now_down, 
	double total_up, double now_up) {

	int bar_width = 25;
	int downloaded_width, i;
	double downloaded;

	downloaded = now_down/total_down;
	downloaded_width = round(downloaded * bar_width);

	i=0;
	printf("[");
	for (; i < downloaded_width; i++) {
		printf("=");
	}
	for (; i < bar_width; i++) {
		printf(" ");
	}

	printf("] %3.0f%% %.0fk\r", downloaded * 100, now_down / 1024/*, total_down / 1024*/);
	fflush(stdout);

	return 0;
}

/**
 * Gets cookies as a wb_str_list from a CURL struct.
 *
 * @param curl - the CURL struct
 * @return a wb_str_list containing cookies on success, NULL
 *   otherwise. IMPORTANT: this list must be freed with
 *   wb_list_free().
 */
struct wb_str_list *
curl_get_cookies(CURL *curl) {
	CURLcode res;
	struct wb_str_list *cookies;

	res = curl_easy_getinfo(curl, CURLINFO_COOKIELIST, &cookies);
	if (res != CURLE_OK) {
		if (cookies != NULL) {
			wb_list_free(cookies);
		}

		return NULL;
	}

	return cookies;
}

/**
 * Adds all cookies from a wb_str_list to a CURL structure.
 *
 * @param curl - the CURL structure to add cookies to.
 * @param cookies - a wb_str_list containing the cookies to
 *   be added.
 */
void
curl_add_cookies(CURL *curl, struct wb_str_list *cookies) {
	struct wb_str_list *cookie = cookies;
	while (cookie != NULL) {
		curl_easy_setopt(curl, CURLOPT_COOKIELIST, cookie->str);
		cookie = cookie->next;
	}
}

/**
 * Connects to URL with a GET or POST request and returns
 * the response.
 *
 * @param url - the URL to connect to.
 * @param post_data (optional) - the post data as a string.
 *   If specified, a POST request is made. If post is not
 *   needed, pass NULL here and a GET request will be made.
 * @param cookies (optional) - the cookies to use for this
 *   request.
 * @param update_cookies - 1 if you need the cookies to be
 *   updated, 0 to leave the cookies as they were.
 * @return the response as a string on success, NULL otherwise.
 *   IMPORTANT: the returned string should be freed with free().
 */
char *
net_get_response(const char *url, const char *post_data,
	struct wb_str_list **cookies, int update_cookies) {

	CURLcode res;

	/* Set up struct for CURL response */
	struct curl_response response;
	response.size = 0;
	response.data = malloc(4096);
	if (response.data == NULL) {
		return NULL;
	}
	response.data[0] = '\0';

	/* Set up CURL */
	setup_curl_handle();
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_data_to_response);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1L);

	if (post_data != NULL) {
		curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post_data);
	}

	if (cookies != NULL) {
		curl_add_cookies(curl_handle, *cookies);
	}

	/* Perform CURL transaction */
	res = curl_easy_perform(curl_handle);
	if (res != CURLE_OK) {
		return NULL;
	}

	/* Update cookies if needed */
	if (update_cookies == 1) {
		if (cookies != NULL) {
			wb_list_free(*cookies);
		}

		*cookies = curl_get_cookies(curl_handle);
		if (*cookies == NULL) {
			return NULL;
		}
	}
	
	return response.data;
}

/**
 * Downloads a file from the specified URL.
 *
 * @param url - the URL to download from.
 * @param file_path - path of the downloaded file.
 * @param file_name - name of the file to download. If this is NULL
 *   then the filename from the URL is used.
 * @return 0 on success, -1 otherwise.
 */
int
net_download(const char *url, const char *file_path, const char *file_name) {
	CURLcode res;
	char *full_file_path;
	char *url_copy = NULL;
	int path_length, full_path_length;
	FILE *download_file;

	/* Get the full file path to download to */
	if (file_name == NULL) {
		url_copy = strdup(url);
		file_name = basename(url_copy);
	}

	path_length = strlen(file_path);
	full_path_length = path_length + strlen(file_name) + 2;
	full_file_path = (char *) malloc(full_path_length);

	if ((path_length > 0) && (file_path[path_length - 1] != '/')) {
		snprintf(full_file_path, full_path_length, "%s/%s", file_path, file_name);
	} else {
		snprintf(full_file_path, full_path_length, "%s%s", file_path, file_name);
	}

	if (url_copy != NULL) {
		free(url_copy);
	}

	/* Open the file */
	download_file = fopen(full_file_path, "wb");

	if (download_file == NULL) {
		free(full_file_path);
		return -1;
	}

	/* Set up CURL */
	setup_curl_handle();
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_data_to_file);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, download_file);
	curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl_handle, CURLOPT_PROGRESSFUNCTION, download_progress);

	/* Download file */
	res = curl_easy_perform(curl_handle);
	printf("\n");

	/* Close the file */
	if (fclose(download_file) != 0) {
		free(full_file_path);
		return -1;
	}

	/* Check if CURL encountered errors */
	if (res != CURLE_OK) {
		remove(full_file_path);
		free(full_file_path);
		return -1;
	}

	/* Final cleanup */
	free(full_file_path);

	return 0;
}

/**
 * Downloads a list of files to the specified directory.
 *
 * @param urls - a list of URLs of files to download
 * @param dir - the dir to download to
 * @return 0 on success, -1 otherwise
 */
int
net_download_list(struct wb_str_list *urls, const char *dir) {
	struct wb_str_list *url;
	int failed = 0;

	url = urls;
	while (url != NULL) {
		if (net_download(url->str, dir, NULL) != 0) {
			failed++;
		}
		url = url->next;
	}

	return (failed == 0) ? 0 : -1;
}

/**
 * A wrapper for net_get_response(). Should be used when the
 * response is not needed.
 *
 * @return 0 on success, -1 otherwise.
 *
 * @see net_get_response()
 */
int
net_connect(const char *url, const char *post_data,
	struct wb_str_list **cookies, int update_cookies) {

	char *response;

	response = net_get_response(url, post_data, cookies, update_cookies);
	if (response == NULL) {
		return -1;
	}

	free(response);
	return 0;
}
