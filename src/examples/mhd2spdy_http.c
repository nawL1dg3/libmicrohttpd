/*
    Copyright (C) 2013 Andrey Uzunov

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * @file mhd2spdy_http.c
 * @brief  HTTP part of the proxy. libmicrohttpd is used for the server side.
 * @author Andrey Uzunov
 */
 
#include "mhd2spdy_structures.h"
#include "mhd2spdy_http.h"
#include "mhd2spdy_spdy.h"


void *
http_log_cb(void * cls,
const char * uri)
{
  (void)cls;
  
  struct HTTP_URI * http_uri;
  
  PRINT_INFO2("log uri '%s'\n", uri);
  
  if(NULL == (http_uri = au_malloc(sizeof(struct HTTP_URI ))))
    DIE("no memory");
  http_uri->uri = strdup(uri);
  return http_uri;
}


static int
http_iterate_cb(void *cls,
                 enum MHD_ValueKind kind,
                 const char *name,
                 const char *value)
{
  (void)kind;
  
  static char * const forbidden[] = {"Transfer-Encoding",
    "Proxy-Connection",
    "Keep-Alive",
    "Connection"};
  static int forbidden_size = 4;
  int i;
	struct SPDY_Headers *spdy_headers = (struct SPDY_Headers *)cls;
	
	if(0 == strcasecmp(name, "Host"))
    spdy_headers->nv[9] = (char *)value;
  else
  {
    for(i=0; i<forbidden_size; ++i)
      if(0 == strcasecmp(forbidden[i], name))
        return MHD_YES;
    spdy_headers->nv[spdy_headers->cnt++] = (char *)name;
    spdy_headers->nv[spdy_headers->cnt++] = (char *)value;
  }
	
	return MHD_YES;
}


static ssize_t
http_response_callback (void *cls,
                        uint64_t pos,
                        char *buffer,
                        size_t max)
{
  (void)pos;
  
	int ret;
	struct Proxy *proxy = (struct Proxy *)cls;
	void *newbody;
  const union MHD_ConnectionInfo *info;
  int val = 1;
  
  PRINT_INFO2("http_response_callback for %s", proxy->url);
  
  if(proxy->error)
    return MHD_CONTENT_READER_END_WITH_ERROR;
  
	if(0 == proxy->http_body_size &&( proxy->done || !proxy->spdy_active)){
    PRINT_INFO("sent end of stream");
    return MHD_CONTENT_READER_END_OF_STREAM;
  }
	
	if(!proxy->http_body_size)//nothing to write now
  {
    //flush data
    info = MHD_get_connection_info (proxy->http_connection,
         MHD_CONNECTION_INFO_CONNECTION_FD);
    ret = setsockopt(info->connect_fd, IPPROTO_TCP, TCP_NODELAY, &val, (socklen_t)sizeof(val));
    if(ret == -1) {
      DIE("setsockopt");
    }
    
    PRINT_INFO("FLUSH data");
		return 0;
  }
	
	if(max >= proxy->http_body_size)
	{
		ret = proxy->http_body_size;
		newbody = NULL;
	}
	else
	{
		ret = max;
		if(NULL == (newbody = au_malloc(proxy->http_body_size - max)))
		{
			PRINT_INFO("no memory");
			return -2;
		}
		memcpy(newbody, proxy->http_body + max, proxy->http_body_size - max);
	}
	memcpy(buffer, proxy->http_body, ret);
	free(proxy->http_body);
	proxy->http_body = newbody;
	proxy->http_body_size -= ret;
	
	if(proxy->length >= 0)
	{
		proxy->length -= ret;
	}
	
	PRINT_INFO2("response_callback, size: %i",ret);
	
	return ret;
}


static void
http_response_done_callback(void *cls)
{
	struct Proxy *proxy = (struct Proxy *)cls;
  
  PRINT_INFO2("http_response_done_callback for %s", proxy->url);
  
  if(proxy->spdy_active)
    proxy->http_active = false;
  else
    free_proxy(proxy);

  --glob_opt.responses_pending;
}

int
http_cb_request (void *cls,
                struct MHD_Connection *connection,
                const char *url,
                const char *method,
                const char *version,
                const char *upload_data,
                size_t *upload_data_size,
                void **ptr)
{
  (void)cls;
  (void)url;
  (void)upload_data;
  (void)upload_data_size;
  
  int ret;
  struct Proxy *proxy;
  struct SPDY_Headers spdy_headers;
  
  //PRINT_INFO2("request cb %i; %s", *ptr,url);

  if (NULL == *ptr)
    DIE("ptr is null");
  struct HTTP_URI *http_uri = (struct HTTP_URI *)*ptr;
    
  if(NULL == http_uri->proxy)
  {  
    if (0 != strcmp (method, MHD_HTTP_METHOD_GET))
    {
      free(http_uri->uri);
      free(http_uri);
      PRINT_INFO2("unexpected method %s", method);
      return MHD_NO;
    }
  
    if(NULL == (proxy = au_malloc(sizeof(struct Proxy))))
    {
      PRINT_INFO("No memory");
      return MHD_NO; 
    }
    
    ++glob_opt.responses_pending;
    proxy->id = rand();
    proxy->http_active = true;
    proxy->http_connection = connection;
    http_uri->proxy = proxy;
    return MHD_YES;
  }
  
  proxy = http_uri->proxy;

  if(proxy->spdy_active)
  {
    //already handled
    PRINT_INFO("unnecessary call to http_cb_request");
    return MHD_YES;
  }

  PRINT_INFO2("received request for '%s %s %s'\n", method, http_uri->uri, version);

  proxy->url = http_uri->uri;
    
  ret = parse_uri(&glob_opt.uri_preg, proxy->url, &proxy->uri);
  if(ret != 0)
    DIE("parse_uri failed");
  proxy->http_uri = http_uri;
  proxy->spdy_active = true;

  spdy_headers.num = MHD_get_connection_values (connection,
                       MHD_HEADER_KIND,
                       NULL,
                       NULL);
  if(NULL == (spdy_headers.nv = au_malloc(((spdy_headers.num + 5) * 2 + 1) * sizeof(char *))))
    DIE("no memory");
  spdy_headers.nv[0] = ":method";     spdy_headers.nv[1] = "GET";
  spdy_headers.nv[2] = ":path";       spdy_headers.nv[3] = proxy->uri->path_and_more;
  spdy_headers.nv[4] = ":version";    spdy_headers.nv[5] = (char *)version;
  spdy_headers.nv[6] = ":scheme";     spdy_headers.nv[7] = proxy->uri->scheme;
  spdy_headers.nv[8] = ":host";       spdy_headers.nv[9] = NULL;
  //nv[14] = NULL;
  spdy_headers.cnt = 10;
  MHD_get_connection_values (connection,
                       MHD_HEADER_KIND,
                       &http_iterate_cb,
                       &spdy_headers);
                       
  spdy_headers.nv[spdy_headers.cnt] = NULL;
  if(NULL == spdy_headers.nv[9])
    spdy_headers.nv[9] = proxy->uri->host_and_port;

  if(0 != spdy_request(spdy_headers.nv, proxy))
  {
    free(spdy_headers.nv);
    free_proxy(proxy);
    
    return MHD_NO;
  }
  free(spdy_headers.nv);
  
  proxy->http_response = MHD_create_response_from_callback (MHD_SIZE_UNKNOWN,
                         4096,
                         &http_response_callback,
                         proxy,
                         &http_response_done_callback);

  if (proxy->http_response == NULL)
    DIE("no response");
  
  if(MHD_NO == MHD_add_response_header (proxy->http_response,
                 "Proxy-Connection", "keep-alive"))
    PRINT_INFO("SPDY_name_value_add failed: ");
  if(MHD_NO == MHD_add_response_header (proxy->http_response,
                 "Connection", "Keep-Alive"))
    PRINT_INFO("SPDY_name_value_add failed: ");
  if(MHD_NO == MHD_add_response_header (proxy->http_response,
                 "Keep-Alive", "timeout=5, max=100"))
    PRINT_INFO("SPDY_name_value_add failed: ");
    
  return MHD_YES;
}


void
http_create_response(struct Proxy* proxy,
                     char **nv)
{
  size_t i;
  
  for(i = 0; nv[i]; i += 2) {
    if(0 == strcmp(":status", nv[i]))
    {
      char tmp[4];
      memcpy(&tmp,nv[i+1],3);
      tmp[3]=0;
      proxy->status = atoi(tmp);
      continue;
    }
    else if(0 == strcmp(":version", nv[i]))
    {
      proxy->version = nv[i+1];
      continue;
    }
    else if(0 == strcmp("content-length", nv[i]))
    {
      continue;
    }

    char *header = *(nv+i);
    if(MHD_NO == MHD_add_response_header (proxy->http_response,
                   header, nv[i+1]))
    {
      PRINT_INFO2("SPDY_name_value_add failed: '%s' '%s'", header, nv[i+1]);
    }
    PRINT_INFO2("adding '%s: %s'",header, nv[i+1]);
  }
  
  if(MHD_NO == MHD_queue_response (proxy->http_connection, proxy->status, proxy->http_response)){
    PRINT_INFO("No queue");
    abort();
  }
  
  MHD_destroy_response (proxy->http_response);
}
