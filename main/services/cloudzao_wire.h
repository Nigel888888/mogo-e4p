/*
 * 中文：会话内的 HTTP 报文，不持久化凭据或 URL。
 * English: Session-local HTTP framing; no persistence of credentials or URLs.
 */
#pragma once
#include "cloudzao_receive.h"
#include "http_parser.h"

typedef struct {
    http_parser parser;
    cz_sink_t sink;
    void *context;
    size_t received, limit, error_used;
    char error_json[512];
    cz_error_t error;
    bool done;
    cz_request_t operation;
} cz_response_t;

/// 构造独立请求，返回字节数；失败返回零。/ Compose one isolated request, returning its byte length or zero.
size_t cloudzao_wire_request(cz_request_t operation, const cz_http_request_t *req, char *host, size_t host_cap,
                             char *output, size_t output_cap);
void cloudzao_wire_begin(cz_response_t *response, cz_request_t operation, size_t limit, cz_sink_t sink, void *context);
/// 长度为零表示 EOF；错误正文只供本地状态映射。/ Zero length is EOF; error bodies are only used for status mapping.
bool cloudzao_wire_feed(cz_response_t *response, const void *data, size_t length);
