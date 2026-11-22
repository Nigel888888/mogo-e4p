/*
 * 中文：CloudZao 使用一次请求、一次响应的 HTTPS 连接；无跳转、认证重试或跨主机复用。
 * English: One request/response per HTTPS connection; no redirects, auth retry, or cross-host reuse.
 */
#include "cloudzao_wire.h"
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define CZ_HOST "claw.cloudzao.cn"
#define CZ_ROOT "/xiaozhi/api/partner/v1"

size_t cloudzao_wire_request(cz_request_t operation, const cz_http_request_t *req, char *host, size_t host_cap,
                             char *output, size_t output_cap) {
    const bool download = operation == CZ_DOWNLOAD;
    char ack_path[sizeof(CZ_ROOT "/deliveries//acknowledgements") + CLOUDZAO_DELIVERY_ID_MAX];
    const char *path;
    switch (operation) {
        case CZ_BIND: path = CZ_ROOT "/pairings"; break;
        case CZ_EXCHANGE: path = CZ_ROOT "/device-tokens"; break;
        case CZ_REFRESH: path = CZ_ROOT "/device-tokens/refresh"; break;
        case CZ_LIST: path = CZ_ROOT "/deliveries"; break;
        case CZ_ACK:
            if (!cloudzao_delivery_id_valid(req->delivery_id)) return 0;
            snprintf(ack_path, sizeof(ack_path), CZ_ROOT "/deliveries/%s/acknowledgements", req->delivery_id);
            path = ack_path;
            break;
        case CZ_DOWNLOAD: path = NULL; break;
        default: return 0;
    }
    const bool bearer = operation == CZ_ACK || operation == CZ_LIST;
    const bool json = operation == CZ_BIND || operation == CZ_EXCHANGE || operation == CZ_REFRESH;
    if (bearer != (req->token != NULL) || (bearer && !cloudzao_token_valid(req->token)) ||
        json != (req->body != NULL) || (operation == CZ_ACK) != (req->delivery_id != NULL)) return 0;
    int n;
    size_t used = 0;
    if (download) {
        static const char prefix[] = "GET ";
        static const char hex[] = "0123456789ABCDEF";
        cz_download_target_t target;
        if (!cloudzao_download_target(req->url, &target) || target.host_len >= host_cap ||
            sizeof(prefix) - 1 + target.encoded_len >= output_cap) return 0;
        memcpy(host, target.host, target.host_len);
        host[target.host_len] = 0;
        memcpy(output, prefix, sizeof(prefix) - 1);
        used = sizeof(prefix) - 1;
        // 已有转义和签名参数逐字节保留；只有验证过的 OSS 路径 UTF-8 会编码。
        // Preserve existing escapes and signatures byte-for-byte; encode only validated UTF-8 in an OSS path.
        for (const unsigned char *p = (const unsigned char *)target.path; *p; ++p) {
            if (*p < 128) output[used++] = (char)*p;
            else {
                output[used++] = '%';
                output[used++] = hex[*p >> 4];
                output[used++] = hex[*p & 15];
            }
        }
    } else {
        if (req->url || host_cap < sizeof(CZ_HOST)) return 0;
        memcpy(host, CZ_HOST, sizeof(CZ_HOST));
        n = snprintf(output, output_cap, "%s %s", operation == CZ_LIST ? "GET" : "POST", path);
        if (n < 0 || (size_t)n >= output_cap) return 0;
        used = (size_t)n;
    }
    n = snprintf(output + used, output_cap - used, " HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept-Encoding: identity\r\n", host);
    if (n < 0 || (size_t)n >= output_cap - used) return 0;
    used += (size_t)n;
    if (bearer) {
        n = snprintf(output + used, output_cap - used, "Authorization: Bearer %s\r\n", req->token);
        if (n < 0 || (size_t)n >= output_cap - used) return 0;
        used += (size_t)n;
    }
    n = json ? snprintf(output + used, output_cap - used,
                        "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s", strlen(req->body), req->body) :
        snprintf(output + used, output_cap - used, "%s\r\n", operation == CZ_ACK ? "Content-Length: 0\r\n" : "");
    return n >= 0 && (size_t)n < output_cap - used ? used + (size_t)n : 0;
}

static bool success(const cz_response_t *r) {
    return r->parser.status_code == (r->operation == CZ_ACK ? 204 : 200);
}

static int headers(http_parser *parser) {
    cz_response_t *r = parser->data;
    if (!success(r) && (r->operation == CZ_DOWNLOAD || parser->status_code < 400)) r->error = CZ_SERVER;
    else if (parser->content_length != UINT64_MAX &&
             parser->content_length > (success(r) ? r->limit : sizeof(r->error_json) - 1)) r->error = CZ_INVALID;
    return r->error == CZ_OK ? 0 : -1;
}

static int body(http_parser *parser, const char *data, size_t length) {
    cz_response_t *r = parser->data;
    if (!success(r)) {
        if (length < sizeof(r->error_json) - r->error_used && !memchr(data, 0, length)) {
            memcpy(r->error_json + r->error_used, data, length);
            r->error_used += length;
            return 0;
        }
    } else if (length <= r->limit - r->received && r->sink && r->sink(r->context, data, length)) {
        r->received += length;
        return 0;
    }
    r->error = CZ_INVALID;
    return -1;
}

static cz_error_t business_error(cz_response_t *r) {
    cJSON *root = cloudzao_json_parse(r->error_json, r->error_used);
    const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "error"));
    cz_error_t result = CZ_SERVER;
    unsigned status = r->parser.status_code;
    if (status == 401 && (r->operation == CZ_ACK || r->operation == CZ_LIST || r->operation == CZ_REFRESH))
        result = CZ_AUTH;
    if (code) {
        if (status == 401 && r->operation == CZ_REFRESH && !strcmp(code, "refresh_reused")) result = CZ_REFRESH_REUSED;
        if (status == 409 && r->operation == CZ_BIND && !strcmp(code, "activation_unavailable")) result = CZ_ACTIVATION_UNAVAILABLE;
        if (status == 409 && r->operation == CZ_EXCHANGE && !strcmp(code, "authorization_pending")) result = CZ_PAIR_PENDING;
        if (status == 410 && r->operation == CZ_EXCHANGE && !strcmp(code, "pairing_expired")) result = CZ_PAIR_EXPIRED;
        if (status == 429 && !strcmp(code, "slow_down")) result = CZ_SLOW_DOWN;
        if (status == 404 && r->operation == CZ_BIND && !strcmp(code, "integration_not_found")) result = CZ_INTEGRATION;
        if (status == 404 && r->operation == CZ_ACK && !strcmp(code, "delivery_not_found")) result = CZ_DELIVERY_MISSING;
        if (status == 400 && !strcmp(code, "invalid_request")) result = CZ_INVALID;
    }
    cJSON_Delete(root);
    return result;
}

static int complete(http_parser *parser) {
    cz_response_t *r = parser->data;
    r->done = true;
    if (!success(r)) r->error = business_error(r);
    http_parser_pause(parser, 1);
    return 0;
}

void cloudzao_wire_begin(cz_response_t *r, cz_request_t operation, size_t limit, cz_sink_t sink, void *context) {
    *r = (cz_response_t){.context = context, .operation = operation, .limit = limit, .sink = sink};
    http_parser_init(&r->parser, HTTP_RESPONSE);
    r->parser.data = r;
}

bool cloudzao_wire_feed(cz_response_t *r, const void *data, size_t length) {
    static const http_parser_settings callbacks = {
        .on_headers_complete = headers, .on_body = body, .on_message_complete = complete,
    };
    size_t used = http_parser_execute(&r->parser, &callbacks, data, length);
    enum http_errno error = HTTP_PARSER_ERRNO(&r->parser);
    if ((error == HPE_OK || (error == HPE_PAUSED && r->done)) && used == length &&
        !r->parser.upgrade && (length || r->done)) return true;
    if (!r->done && r->error != CZ_OK) return false;
    r->error = CZ_INVALID;
    return false;
}
