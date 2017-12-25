/*
 +----------------------------------------------------------------------+
 | Zan                                                                  |
 +----------------------------------------------------------------------+
 | Copyright (c) 2012-2016 Swoole Team <http://github.com/swoole>       |
 +----------------------------------------------------------------------+
 | This source file is subject to version 2.0 of the Apache license,    |
 | that is bundled with this package in the file LICENSE, and is        |
 | available through the world-wide-web at the following url:           |
 | http://www.apache.org/licenses/LICENSE-2.0.html                      |
 | If you did not receive a copy of the Apache2.0 license and are unable|
 | to obtain it through the world-wide-web, please send a note to       |
 | license@swoole.com so we can mail you a copy immediately.            |
 +----------------------------------------------------------------------+
 | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
 +----------------------------------------------------------------------+
 */

#include <assert.h>
#include <stddef.h>

#include "swoole.h"
#include "swProtocol/http.h"
#include "swProtocol/http2.h"
#include "swBaseOperator.h"
#include "zanMemory/zanMemory.h"

/**
 * only GET/POST
 */
int swHttpRequest_get_protocol(swHttpRequest *request)
{
    char *buf = request->buffer->str;
    char *pe = buf + request->buffer->length;

    //http method
    if (memcmp(buf, "GET", 3) == 0)
    {
        request->method = HTTP_GET;
        request->offset = 4;
        buf += 4;
    }
    else if (memcmp(buf, "POST", 4) == 0)
    {
        request->method = HTTP_POST;
        request->offset = 5;
        buf += 5;
    }
    else if (memcmp(buf, "PUT", 3) == 0)
    {
        request->method = HTTP_PUT;
        request->offset = 4;
        buf += 4;
    }
    else if (memcmp(buf, "PATCH", 5) == 0)
    {
        request->method = HTTP_PATCH;
        request->offset = 6;
        buf += 6;
    }
    else if (memcmp(buf, "DELETE", 6) == 0)
    {
        request->method = HTTP_DELETE;
        request->offset = 7;
        buf += 7;
    }
    else if (memcmp(buf, "HEAD", 4) == 0)
    {
        request->method = HTTP_HEAD;
        request->offset = 5;
        buf += 5;
    }
    else if (memcmp(buf, "OPTIONS", 7) == 0)
    {
        request->method = HTTP_OPTIONS;
        request->offset = 8;
        buf += 8;
    }
#ifdef SW_USE_HTTP2
    //HTTP2 Connection Preface
    else if (memcmp(buf, "PRI", 3) == 0)
    {
        request->method = HTTP_PRI;
        if (memcmp(buf, SW_HTTP2_PRI_STRING, sizeof(SW_HTTP2_PRI_STRING) - 1) == 0)
        {
            request->buffer->offset = sizeof(SW_HTTP2_PRI_STRING) - 1;
            return ZAN_OK;
        }
        else
        {
            return ZAN_ERR;
        }
    }
#endif
    else
    {
        return ZAN_ERR;
    }

    //http version
    char *p;
    char cmp = 0;
    for (p = buf; p < pe; p++)
    {
        if (cmp == 0 && *p == SW_SPACE)
        {
            cmp = 1;
        }
        else if (cmp == 1)
        {
            if (p + 8 > pe)
            {
                return ZAN_ERR;
            }
            if (memcmp(p, "HTTP/1.1", 8) == 0)
            {
                request->version = HTTP_VERSION_11;
                break;
            }
            else if (memcmp(p, "HTTP/1.0", 8) == 0)
            {
                request->version = HTTP_VERSION_10;
                break;
            }
            else
            {
                return ZAN_ERR;
            }
        }
    }
    p += 8;
    request->buffer->offset = p - request->buffer->str;
    return ZAN_OK;
}

void swHttpRequest_free(swConnection *conn)
{
    swHttpRequest *request = conn->object;
    if (request)
    {
        if (request->buffer)
        {
            swString_free(request->buffer);
        }
        bzero(request, sizeof(swHttpRequest));
        sw_free(request);
        conn->object = NULL;
    }
}

/**
 * POST content-length
 */
int swHttpRequest_get_content_length(swHttpRequest *request)
{
    swString *buffer = request->buffer;
    char *buf = buffer->str + buffer->offset;
    int len = buffer->length - buffer->offset;

    /// note: hantf fix #1030; need to check
    char *pe = buf + len - 2;
    char *p;
    char *eol;

    for (p = buf; p < pe; p++)
    {
        if (*p == '\r' && *(p + 1) == '\n')
        {
            buffer->offset = p - buffer->str;
            if (strncasecmp(p + 2, SW_STRL("Content-Length") - 1) == 0)
            {
                p += 2 + (sizeof("Content-Length:") - 1);
                eol = strstr(p,"\r\n");
                if (NULL == eol)
                {
                    return ZAN_ERR;
                }

                while (isspace(*p) && p != eol)
                {
                    p++;
                }

                if (p == eol)
                {
                    return ZAN_ERR;
                }

                request->content_length = atoi(p);
                buffer->offset = eol - buffer->str;
                return ZAN_OK;
            }
        }
    }

    return ZAN_ERR;
}

#ifdef SW_HTTP_100_CONTINUE
int swHttpRequest_has_expect_header(swHttpRequest *request)
{
    swString *buffer = request->buffer;
    //char *buf = buffer->str + buffer->offset;
    char *buf = buffer->str;
    //int len = buffer->length - buffer->offset;
    int len = buffer->length;

    char *pe = buf + len;
    char *p;

    for (p = buf; p < pe; p++)
    {

        if (*p == '\r' && *(p + 1) == '\n')
        {
            if (strncasecmp(p + 2, SW_STRL("Expect") - 1) == 0)
            {
                p += sizeof("Expect: ") + 1;
                if (strncasecmp(p, SW_STRL("100-continue") - 1) == 0)
                {
                    return 1;
                }
                else
                {
                    return 0;
                }
            }
            else
            {
                p++;
            }
        }
    }
    return 0;
}
#endif

/**
 * POST get header-length
 */
int swHttpRequest_get_header_length(swHttpRequest *request)
{
    swString *buffer = request->buffer;

    int n = swoole_strnpos(buffer->str, buffer->length, "\r\n\r\n", 4);
    if (n < 0)
    {
        return ZAN_ERR;
    }

    request->header_length = n + 4;
    return ZAN_OK;
}
