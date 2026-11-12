/* HTML 词法分析器定义，re2c 4.3 规则，生成结果为 C 源文件。
   仅使用 C 语法：标签、属性、注释、DOCTYPE、正文文本与 script/style 原始文本
   都由下方 re2c 规则识别，实体引用在词法阶段完成解码。 */

#include <stdlib.h>
#include <string.h>

#include "html_parser.h"
#include "html_lexer.h"
#include "html_builder.h"

/* 模式扫描函数返回该值表示模式已切换，主循环需要按新状态重新扫描。 */
#define WEB_HTML_RETRY (-1)

/* ---------------------------------------------------------------- 
   基础字符工具
   ---------------------------------------------------------------- */

static int web_html_is_space(int character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f';
}

static int web_html_lower_char(int character) {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A' + 'a';
    }
    return character;
}

/* 复制 [begin, end) 区间为以 NUL 结尾的新字符串。 */
static char* web_html_strdup_range(const char* begin, const char* end) {
    size_t length = (size_t)(end - begin);
    char* text = (char*)malloc(length + 1);
    if (text == NULL) {
        return NULL;
    }
    if (length > 0) {
        memcpy(text, begin, length);
    }
    text[length] = '\0';
    return text;
}

/* 复制区间并转成 ASCII 小写，用于标签名与属性名。 */
static char* web_html_strdup_lower(const char* begin, const char* end) {
    size_t length = (size_t)(end - begin);
    char* text = (char*)malloc(length + 1);
    if (text == NULL) {
        return NULL;
    }
    size_t index = 0;
    for (; index < length; ++index) {
        text[index] = (char)web_html_lower_char((unsigned char)begin[index]);
    }
    text[length] = '\0';
    return text;
}

/* ---------------------------------------------------------------- 
   可变长字符缓冲，用于实体解码结果
   ---------------------------------------------------------------- */

struct WebHtmlBuffer {
    char* data;
    size_t length;
    size_t capacity;
};

static int web_html_buffer_reserve(struct WebHtmlBuffer* buffer, size_t extra) {
    if (buffer->length + extra <= buffer->capacity) {
        return 1;
    }
    size_t capacity = buffer->capacity == 0 ? 32 : buffer->capacity;
    while (capacity < buffer->length + extra) {
        capacity *= 2;
    }
    char* data = (char*)realloc(buffer->data, capacity);
    if (data == NULL) {
        return 0;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

static int web_html_buffer_append(struct WebHtmlBuffer* buffer, const char* begin,
                                  const char* end) {
    size_t length = (size_t)(end - begin);
    if (!web_html_buffer_reserve(buffer, length)) {
        return 0;
    }
    if (length > 0) {
        memcpy(buffer->data + buffer->length, begin, length);
    }
    buffer->length += length;
    return 1;
}

static int web_html_buffer_append_char(struct WebHtmlBuffer* buffer, int character) {
    if (!web_html_buffer_reserve(buffer, 1)) {
        return 0;
    }
    buffer->data[buffer->length] = (char)character;
    buffer->length += 1;
    return 1;
}

static int web_html_buffer_append_utf8(struct WebHtmlBuffer* buffer, unsigned int codepoint) {
    char bytes[4];
    size_t count = 0;
    if (codepoint <= 0x7FU) {
        bytes[0] = (char)codepoint;
        count = 1;
    } else if (codepoint <= 0x7FFU) {
        bytes[0] = (char)(0xC0U | (codepoint >> 6));
        bytes[1] = (char)(0x80U | (codepoint & 0x3FU));
        count = 2;
    } else if (codepoint <= 0xFFFFU) {
        bytes[0] = (char)(0xE0U | (codepoint >> 12));
        bytes[1] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        bytes[2] = (char)(0x80U | (codepoint & 0x3FU));
        count = 3;
    } else {
        bytes[0] = (char)(0xF0U | (codepoint >> 18));
        bytes[1] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
        bytes[2] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        bytes[3] = (char)(0x80U | (codepoint & 0x3FU));
        count = 4;
    }
    if (!web_html_buffer_reserve(buffer, count)) {
        return 0;
    }
    memcpy(buffer->data + buffer->length, bytes, count);
    buffer->length += count;
    return 1;
}

static char* web_html_buffer_detach(struct WebHtmlBuffer* buffer) {
    if (!web_html_buffer_reserve(buffer, 1)) {
        free(buffer->data);
        buffer->data = NULL;
        buffer->length = 0;
        buffer->capacity = 0;
        return NULL;
    }
    buffer->data[buffer->length] = '\0';
    char* data = buffer->data;
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    return data;
}

/* ---------------------------------------------------------------- 
   HTML 实体解码
   ---------------------------------------------------------------- */

struct WebHtmlEntityName {
    const char* name;
    size_t length;
    unsigned int codepoint;
};

/* 支持的命名实体；未列出的实体保持原样。 */
static const struct WebHtmlEntityName kWebHtmlEntities[] = {
    {"amp", 3, 0x0026U},    {"lt", 2, 0x003CU},     {"gt", 2, 0x003EU},
    {"quot", 4, 0x0022U},   {"apos", 4, 0x0027U},   {"nbsp", 4, 0x00A0U},
    {"copy", 4, 0x00A9U},   {"reg", 3, 0x00AEU},    {"mdash", 5, 0x2014U},
    {"ndash", 5, 0x2013U},  {"hellip", 6, 0x2026U}, {"ldquo", 5, 0x201CU},
    {"rdquo", 5, 0x201DU},  {"lsquo", 5, 0x2018U},  {"rsquo", 5, 0x2019U},
    {"times", 5, 0x00D7U},  {"middot", 6, 0x00B7U}, {"bull", 4, 0x2022U},
    {"deg", 3, 0x00B0U},    {"trade", 5, 0x2122U},  {"euro", 4, 0x20ACU},
    /* 箭头、引号、标点与常用符号：落地页大量使用这些实体。
       实体名在匹配前统一小写化，因此表中只登记小写形式。 */
    {"rarr", 4, 0x2192U},   {"larr", 4, 0x2190U},   {"uarr", 4, 0x2191U},
    {"darr", 4, 0x2193U},   {"laquo", 5, 0x00ABU},  {"raquo", 5, 0x00BBU},
    {"ensp", 4, 0x2002U},   {"emsp", 4, 0x2003U},   {"thinsp", 6, 0x2009U},
    {"divide", 6, 0x00F7U},  {"plusmn", 6, 0x00B1U},  {"frac12", 6, 0x00BDU},
    {"frac14", 6, 0x00BCU},  {"frac34", 6, 0x00BEU},  {"sup2", 4, 0x00B2U},
    {"sup3", 4, 0x00B3U},    {"micro", 5, 0x00B5U},   {"para", 4, 0x00B6U},
    {"sect", 4, 0x00A7U},    {"cent", 4, 0x00A2U},    {"pound", 5, 0x00A3U},
    {"yen", 3, 0x00A5U},     {"szlig", 5, 0x00DFU},   {"permil", 6, 0x2030U},
    {"prime", 5, 0x2032U},   {"dagger", 6, 0x2020U},  {"star", 4, 0x2606U},
    {"check", 5, 0x2713U},   {"cross", 5, 0x2717U},   {"sol", 3, 0x002FU},
};

static int web_html_entity_name_equals(const char* begin, const char* end, const char* name,
                                       size_t length) {
    if ((size_t)(end - begin) != length) {
        return 0;
    }
    size_t index = 0;
    for (; index < length; ++index) {
        if (web_html_lower_char((unsigned char)begin[index]) != name[index]) {
            return 0;
        }
    }
    return 1;
}

/* 解析十进制或十六进制数字实体，begin 指向 '#'。 */
static int web_html_parse_numeric_entity(const char* begin, const char* end,
                                         unsigned int* codepoint) {
    const char* cursor = begin + 1;
    int hexadecimal = 0;
    if (cursor < end && (*cursor == 'x' || *cursor == 'X')) {
        hexadecimal = 1;
        ++cursor;
    }
    if (cursor >= end) {
        return 0;
    }
    unsigned int value = 0;
    size_t digits = 0;
    while (cursor < end) {
        int character = (unsigned char)*cursor;
        unsigned int digit = 0;
        if (character >= '0' && character <= '9') {
            digit = (unsigned int)(character - '0');
        } else if (hexadecimal && character >= 'a' && character <= 'f') {
            digit = (unsigned int)(character - 'a' + 10);
        } else if (hexadecimal && character >= 'A' && character <= 'F') {
            digit = (unsigned int)(character - 'A' + 10);
        } else {
            return 0;
        }
        if (value > 0x10FFFFU / 16U) {
            return 0;
        }
        value = value * (hexadecimal ? 16U : 10U) + digit;
        if (value > 0x10FFFFU) {
            return 0;
        }
        digits += 1;
        ++cursor;
    }
    if (digits == 0 || value == 0U) {
        return 0;
    }
    if (value >= 0xD800U && value <= 0xDFFFU) {
        return 0;
    }
    *codepoint = value;
    return 1;
}

char* webhtml_decode_entities(const char* begin, const char* end) {
    struct WebHtmlBuffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    const char* cursor = begin;
    while (cursor < end) {
        if (*cursor != '&') {
            const char* run_begin = cursor;
            while (cursor < end && *cursor != '&') {
                ++cursor;
            }
            if (!web_html_buffer_append(&buffer, run_begin, cursor)) {
                free(buffer.data);
                return NULL;
            }
            continue;
        }

        /* 实体名称最长约十余个字符，限制扫描窗口避免长距离回溯。 */
        const char* terminator = NULL;
        const char* scan = cursor + 1;
        const char* window_end = end;
        if ((size_t)(end - scan) > 32) {
            window_end = scan + 32;
        }
        while (scan < window_end) {
            int character = (unsigned char)*scan;
            if (character == ';') {
                terminator = scan;
                break;
            }
            if (character == '#') {
                ++scan;
                continue;
            }
            if (!((character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z'))) {
                break;
            }
            ++scan;
        }

        unsigned int codepoint = 0;
        int recognized = 0;
        if (terminator != NULL && terminator > cursor + 1) {
            const char* name_begin = cursor + 1;
            if (*name_begin == '#') {
                if (web_html_parse_numeric_entity(name_begin, terminator, &codepoint)) {
                    recognized = 1;
                }
            } else {
                size_t index = 0;
                size_t count = sizeof(kWebHtmlEntities) / sizeof(kWebHtmlEntities[0]);
                for (; index < count; ++index) {
                    if (web_html_entity_name_equals(name_begin, terminator,
                                                    kWebHtmlEntities[index].name,
                                                    kWebHtmlEntities[index].length)) {
                        codepoint = kWebHtmlEntities[index].codepoint;
                        recognized = 1;
                        break;
                    }
                }
            }
        }

        if (recognized) {
            if (!web_html_buffer_append_utf8(&buffer, codepoint)) {
                free(buffer.data);
                return NULL;
            }
            cursor = terminator + 1;
        } else {
            /* 未识别的实体按原样保留。 */
            if (!web_html_buffer_append_char(&buffer, '&')) {
                free(buffer.data);
                return NULL;
            }
            cursor += 1;
        }
    }
    return web_html_buffer_detach(&buffer);
}

void webhtml_free_string(char* memory) {
    free(memory);
}

/* ---------------------------------------------------------------- 
   解析状态的生命周期管理
   ---------------------------------------------------------------- */

void webhtml_token_reset(struct WebHtmlParseState* state) {
    if (state == NULL) {
        return;
    }
    free(state->token.name);
    free(state->token.value);
    state->token.name = NULL;
    state->token.value = NULL;
    state->token.begin = 0;
    state->token.end = 0;
    state->token.line = 0;
}

void webhtml_pending_reset(struct WebHtmlParseState* state) {
    if (state == NULL) {
        return;
    }
    int index = 0;
    for (; index < state->attribute_count; ++index) {
        free(state->attribute_names[index]);
        free(state->attribute_values[index]);
    }
    free(state->attribute_names);
    free(state->attribute_values);
    state->attribute_names = NULL;
    state->attribute_values = NULL;
    state->attribute_count = 0;
    state->attribute_capacity = 0;
    free(state->open_tag_name);
    state->open_tag_name = NULL;
}

void webhtml_state_reset(struct WebHtmlParseState* state) {
    if (state == NULL) {
        return;
    }
    webhtml_token_reset(state);
    webhtml_pending_reset(state);
    free(state->end_tag_name);
    state->end_tag_name = NULL;
    state->raw_text_tag[0] = '\0';
    state->pending_raw_text_tag[0] = '\0';
    state->in_tag = 0;
    state->in_end_tag = 0;
}

static int web_html_reserve_attributes(struct WebHtmlParseState* state, int needed) {
    if (needed <= state->attribute_capacity) {
        return 1;
    }
    int capacity = state->attribute_capacity == 0 ? 8 : state->attribute_capacity;
    while (capacity < needed) {
        capacity *= 2;
    }
    char** names = (char**)realloc(state->attribute_names, (size_t)capacity * sizeof(char*));
    if (names == NULL) {
        return 0;
    }
    state->attribute_names = names;
    char** values = (char**)realloc(state->attribute_values, (size_t)capacity * sizeof(char*));
    if (values == NULL) {
        return 0;
    }
    state->attribute_values = values;
    state->attribute_capacity = capacity;
    return 1;
}

void webhtml_pending_add(struct WebHtmlParseState* state, char** name, char** value) {
    if (state == NULL || name == NULL || value == NULL) {
        return;
    }
    if (*name == NULL || *value == NULL) {
        free(*name);
        free(*value);
        *name = NULL;
        *value = NULL;
        return;
    }
    if (!web_html_reserve_attributes(state, state->attribute_count + 1)) {
        free(*name);
        free(*value);
        *name = NULL;
        *value = NULL;
        state->failed = 1;
        return;
    }
    state->attribute_names[state->attribute_count] = *name;
    state->attribute_values[state->attribute_count] = *value;
    state->attribute_count += 1;
    *name = NULL;
    *value = NULL;
}

/* ---------------------------------------------------------------- 
   位置与行号
   ---------------------------------------------------------------- */

static void web_html_update_line(struct WebHtmlParseState* state, const char* begin,
                                 const char* end) {
    const char* cursor = begin;
    for (; cursor < end; ++cursor) {
        if (*cursor == '\n') {
            state->line += 1;
        }
    }
}

/* 记录记号区间、起始行号并推进扫描位置。 */
static void web_html_commit_range(struct WebHtmlParseState* state, const char* begin,
                                  const char* end) {
    state->token.begin = (size_t)(begin - state->input);
    state->token.end = (size_t)(end - state->input);
    state->token.line = state->line;
    web_html_update_line(state, begin, end);
    state->cursor = end;
}

/* ---------------------------------------------------------------- 
   原始文本（script/style）与注释的边界查找
   ---------------------------------------------------------------- */

/* 查找注释结束标记 "-->" 的起始位置，找不到返回 NULL。 */
static const char* web_html_find_comment_end(const char* cursor, const char* limit) {
    const char* position = cursor;
    while (position + 3 <= limit) {
        if (position[0] == '-' && position[1] == '-' && position[2] == '>') {
            return position;
        }
        ++position;
    }
    return NULL;
}

/* 查找 DOCTYPE 等声明的收尾 '>'，忽略成对引号内的 '>'。 */
static const char* web_html_find_declaration_end(const char* cursor, const char* limit) {
    int quote = 0;
    const char* position = cursor;
    for (; position < limit; ++position) {
        int character = (unsigned char)*position;
        if (quote != 0) {
            if (character == quote) {
                quote = 0;
            }
            continue;
        }
        if (character == '"' || character == '\'') {
            quote = character;
            continue;
        }
        if (character == '>') {
            return position;
        }
    }
    return NULL;
}

/* 在原始文本中查找 "</tag" 的位置，tag 为小写标签名；找不到返回 NULL。 */
static const char* web_html_find_raw_text_end(const char* cursor, const char* limit,
                                              const char* tag) {
    size_t tag_length = strlen(tag);
    const char* position = cursor;
    while (position + tag_length + 2 <= limit) {
        if (position[0] == '<' && position[1] == '/') {
            size_t index = 0;
            int matched = 1;
            for (; index < tag_length; ++index) {
                if (web_html_lower_char((unsigned char)position[2 + index]) !=
                    web_html_lower_char((unsigned char)tag[index])) {
                    matched = 0;
                    break;
                }
            }
            if (matched) {
                const char* after = position + 2 + tag_length;
                if (after >= limit || web_html_is_space((unsigned char)*after) ||
                    *after == '>' || *after == '/') {
                    return position;
                }
            }
        }
        ++position;
    }
    return NULL;
}

/* ---------------------------------------------------------------- 
   属性切分
   ---------------------------------------------------------------- */

/* 从已匹配的属性片段中切出名称与值，解码实体并写入当前记号负载。
   片段形式为 name、name=value、name="value" 或 name='value'。 */
static void web_html_emit_attribute(struct WebHtmlParseState* state, const char* begin,
                                    const char* end) {
    const char* cursor = begin;
    while (cursor < end && *cursor != '=' && !web_html_is_space((unsigned char)*cursor)) {
        ++cursor;
    }
    const char* name_end = cursor;

    const char* value_begin = NULL;
    const char* value_end = NULL;
    while (cursor < end && web_html_is_space((unsigned char)*cursor)) {
        ++cursor;
    }
    if (cursor < end && *cursor == '=') {
        ++cursor;
        while (cursor < end && web_html_is_space((unsigned char)*cursor)) {
            ++cursor;
        }
        if (cursor < end && (*cursor == '"' || *cursor == '\'')) {
            int quote = (unsigned char)*cursor;
            ++cursor;
            value_begin = cursor;
            while (cursor < end && (unsigned char)*cursor != quote) {
                ++cursor;
            }
            value_end = cursor;
        } else {
            value_begin = cursor;
            while (cursor < end && !web_html_is_space((unsigned char)*cursor)) {
                ++cursor;
            }
            value_end = cursor;
        }
    }

    char* name = web_html_strdup_lower(begin, name_end);
    char* value = value_begin != NULL ? webhtml_decode_entities(value_begin, value_end)
                                      : web_html_strdup_range(begin, begin);
    if (name == NULL || value == NULL) {
        free(name);
        free(value);
        state->token.name = NULL;
        state->token.value = NULL;
        state->failed = 1;
        return;
    }
    state->token.name = name;
    state->token.value = value;
}

static int web_html_finish_attribute(struct WebHtmlParseState* state, const char* end) {
    const char* begin = state->cursor;
    web_html_emit_attribute(state, begin, end);
    web_html_commit_range(state, begin, end);
    if (state->failed) {
        return HTML_TOK_EOF;
    }
    return HTML_TOK_ATTR;
}

/* ---------------------------------------------------------------- 
   原始文本扫描（script/style 内容按原样提交，不解码实体）
   ---------------------------------------------------------------- */

static int web_html_scan_raw_text(struct WebHtmlParseState* state) {
    const char* begin = state->cursor;
    const char* stop = web_html_find_raw_text_end(begin, state->limit, state->raw_text_tag);
    if (stop == NULL) {
        stop = state->limit;
    }
    if (stop > begin) {
        state->token.value = web_html_strdup_range(begin, stop);
        web_html_commit_range(state, begin, stop);
        if (stop == state->limit) {
            state->raw_text_tag[0] = '\0';
        }
        if (state->token.value == NULL) {
            state->failed = 1;
            return HTML_TOK_EOF;
        }
        return HTML_TOK_TEXT;
    }
    state->raw_text_tag[0] = '\0';
    return WEB_HTML_RETRY;
}

/* ---------------------------------------------------------------- 
   普通模式：注释、DOCTYPE、标签与正文
   ---------------------------------------------------------------- */

static int web_html_scan_normal(struct WebHtmlParseState* state) {
    for (;;) {
        if (state->cursor >= state->limit) {
            return HTML_TOK_EOF;
        }
        const unsigned char* YYCURSOR = (const unsigned char*)state->cursor;
        const unsigned char* YYMARKER;
        (void)YYMARKER;
        /*!re2c
            re2c:define:YYCTYPE = "unsigned char";
            re2c:yyfill:enable = 0;

            TAG_START = [A-Za-z];
            TAG_CHAR = [A-Za-z0-9_.:-];

            "<!--" {
                const char* begin = (const char*)state->cursor;
                const char* body_begin = begin + 4;
                const char* terminator = web_html_find_comment_end(body_begin, state->limit);
                const char* body_end = terminator != NULL ? terminator : state->limit;
                const char* commit_end = terminator != NULL ? terminator + 3 : state->limit;
                state->token.value = web_html_strdup_range(body_begin, body_end);
                web_html_commit_range(state, begin, commit_end);
                if (state->token.value == NULL) {
                    state->failed = 1;
                    return HTML_TOK_EOF;
                }
                return HTML_TOK_COMMENT;
            }

            "<!" [dD] [oO] [cC] [tT] [yY] [pP] [eE] {
                const char* begin = (const char*)state->cursor;
                const char* content_begin = (const char*)YYCURSOR;
                const char* terminator = web_html_find_declaration_end(content_begin, state->limit);
                const char* content_end = terminator != NULL ? terminator : state->limit;
                const char* commit_end = terminator != NULL ? terminator + 1 : state->limit;
                const char* trim_begin = content_begin;
                const char* trim_end = content_end;
                while (trim_begin < trim_end &&
                       web_html_is_space((unsigned char)*trim_begin)) {
                    ++trim_begin;
                }
                while (trim_end > trim_begin &&
                       web_html_is_space((unsigned char)*(trim_end - 1))) {
                    --trim_end;
                }
                state->token.value = web_html_strdup_range(trim_begin, trim_end);
                web_html_commit_range(state, begin, commit_end);
                if (state->token.value == NULL) {
                    state->failed = 1;
                    return HTML_TOK_EOF;
                }
                return HTML_TOK_DOCTYPE;
            }

            "<!" {
                const char* begin = (const char*)state->cursor;
                web_html_commit_range(state, begin, (const char*)YYCURSOR);
                webhtml_builder_note_error(state, "忽略无法识别的声明片段");
                return HTML_TOK_BAD;
            }

            "</" [ \t\r\n\f]* TAG_START TAG_CHAR* {
                const char* begin = (const char*)state->cursor;
                const char* end = (const char*)YYCURSOR;
                const char* name_begin = begin + 2;
                while (name_begin < end && web_html_is_space((unsigned char)*name_begin)) {
                    ++name_begin;
                }
                free(state->end_tag_name);
                state->end_tag_name = web_html_strdup_lower(name_begin, end);
                state->in_end_tag = 1;
                state->in_tag = 0;
                web_html_commit_range(state, begin, end);
                if (state->end_tag_name == NULL) {
                    state->failed = 1;
                    return HTML_TOK_EOF;
                }
                return HTML_TOK_END_TAG_OPEN;
            }

            "</" {
                const char* begin = (const char*)state->cursor;
                web_html_commit_range(state, begin, (const char*)YYCURSOR);
                webhtml_builder_note_error(state, "忽略无法识别的结束标签片段");
                return HTML_TOK_BAD;
            }

            "<" TAG_START TAG_CHAR* {
                const char* begin = (const char*)state->cursor;
                const char* end = (const char*)YYCURSOR;
                webhtml_pending_reset(state);
                state->open_tag_begin = (size_t)(begin - state->input);
                state->open_tag_line = state->line;
                state->open_tag_end = 0;
                state->open_tag_name = web_html_strdup_lower(begin + 1, end);
                state->pending_raw_text_tag[0] = '\0';
                if (state->open_tag_name != NULL) {
                    if (strcmp(state->open_tag_name, "script") == 0 ||
                        strcmp(state->open_tag_name, "style") == 0) {
                        memcpy(state->pending_raw_text_tag, state->open_tag_name,
                               strlen(state->open_tag_name) + 1);
                    }
                }
                state->in_tag = 1;
                state->in_end_tag = 0;
                web_html_commit_range(state, begin, end);
                if (state->open_tag_name == NULL) {
                    state->failed = 1;
                    return HTML_TOK_EOF;
                }
                return HTML_TOK_START_TAG_OPEN;
            }

            [^<\x00]+ {
                const char* begin = (const char*)state->cursor;
                const char* end = (const char*)YYCURSOR;
                state->token.value = webhtml_decode_entities(begin, end);
                web_html_commit_range(state, begin, end);
                if (state->token.value == NULL) {
                    state->failed = 1;
                    return HTML_TOK_EOF;
                }
                return HTML_TOK_TEXT;
            }

            "<" {
                const char* begin = (const char*)state->cursor;
                state->token.value = web_html_strdup_range(begin, (const char*)YYCURSOR);
                web_html_commit_range(state, begin, (const char*)YYCURSOR);
                if (state->token.value == NULL) {
                    state->failed = 1;
                    return HTML_TOK_EOF;
                }
                return HTML_TOK_TEXT;
            }

            [\x00] {
                /* 输入中的空字符（NUL）不属于正文，直接跳过以免打断扫描。
                   缓冲区末尾的哨兵字节不会进入这里：主循环先检查 limit。 */
                web_html_commit_range(state, (const char*)state->cursor,
                                      (const char*)YYCURSOR);
                continue;
            }
        */
    }
}

/* ---------------------------------------------------------------- 
   开始标签体：属性列表与标签结束符
   ---------------------------------------------------------------- */

static int web_html_scan_tag(struct WebHtmlParseState* state) {
    for (;;) {
        if (state->cursor >= state->limit) {
            return HTML_TOK_EOF;
        }
        const unsigned char* YYCURSOR = (const unsigned char*)state->cursor;
        const unsigned char* YYMARKER;
        (void)YYMARKER;
        /*!re2c
            re2c:define:YYCTYPE = "unsigned char";
            re2c:yyfill:enable = 0;

            WS = [ \t\r\n\f];
            ATTR_NAME = [^ \t\r\n\f/>"'=<\x00]+;
            UNQUOTED = [^ \t\r\n\f>"'=<\x00]+;

            WS+ {
                web_html_commit_range(state, (const char*)state->cursor,
                                      (const char*)YYCURSOR);
                continue;
            }

            "/>" {
                const char* begin = (const char*)state->cursor;
                state->in_tag = 0;
                state->pending_raw_text_tag[0] = '\0';
                web_html_commit_range(state, begin, (const char*)YYCURSOR);
                state->open_tag_end = (size_t)(state->cursor - state->input);
                return HTML_TOK_SELF_CLOSE;
            }

            ">" {
                const char* begin = (const char*)state->cursor;
                state->in_tag = 0;
                if (state->pending_raw_text_tag[0] != '\0') {
                    size_t length = strlen(state->pending_raw_text_tag);
                    memcpy(state->raw_text_tag, state->pending_raw_text_tag, length + 1);
                }
                state->pending_raw_text_tag[0] = '\0';
                web_html_commit_range(state, begin, (const char*)YYCURSOR);
                state->open_tag_end = (size_t)(state->cursor - state->input);
                return HTML_TOK_TAG_CLOSE;
            }

            "/" {
                web_html_commit_range(state, (const char*)state->cursor,
                                      (const char*)YYCURSOR);
                continue;
            }

            "<" {
                /* 标签尚未收尾就出现新的开始标签：丢弃当前标签并重新扫描。 */
                const char* begin = (const char*)state->cursor;
                state->in_tag = 0;
                state->open_tag_begin = 0;
                state->open_tag_end = 0;
                state->pending_raw_text_tag[0] = '\0';
                webhtml_pending_reset(state);
                webhtml_builder_note_error(state, "标签内出现新的开始标签，前一个标签被丢弃");
                state->cursor = begin;
                return WEB_HTML_RETRY;
            }

            ATTR_NAME WS* "=" WS* "\"" [^"\x00]* "\"" {
                return web_html_finish_attribute(state, (const char*)YYCURSOR);
            }

            ATTR_NAME WS* "=" WS* "'" [^'\x00]* "'" {
                return web_html_finish_attribute(state, (const char*)YYCURSOR);
            }

            ATTR_NAME WS* "=" WS* UNQUOTED+ {
                return web_html_finish_attribute(state, (const char*)YYCURSOR);
            }

            ATTR_NAME {
                return web_html_finish_attribute(state, (const char*)YYCURSOR);
            }

            ["'=\x00] {
                web_html_commit_range(state, (const char*)state->cursor,
                                      (const char*)YYCURSOR);
                webhtml_builder_note_error(state, "忽略开始标签内无法识别的字符");
                continue;
            }
        */
    }
}

/* ---------------------------------------------------------------- 
   结束标签体：只等待 '>'
   ---------------------------------------------------------------- */

static int web_html_scan_end_tag(struct WebHtmlParseState* state) {
    for (;;) {
        if (state->cursor >= state->limit) {
            return HTML_TOK_EOF;
        }
        const unsigned char* YYCURSOR = (const unsigned char*)state->cursor;
        const unsigned char* YYMARKER;
        (void)YYMARKER;
        /*!re2c
            re2c:define:YYCTYPE = "unsigned char";
            re2c:yyfill:enable = 0;

            WS+ {
                web_html_commit_range(state, (const char*)state->cursor,
                                      (const char*)YYCURSOR);
                continue;
            }

            ">" {
                const char* begin = (const char*)state->cursor;
                state->in_end_tag = 0;
                web_html_commit_range(state, begin, (const char*)YYCURSOR);
                return HTML_TOK_TAG_CLOSE;
            }

            [^ \t\r\n\f>\x00] {
                web_html_commit_range(state, (const char*)state->cursor,
                                      (const char*)YYCURSOR);
                webhtml_builder_note_error(state, "忽略结束标签内的多余字符");
                continue;
            }

            [\x00] {
                web_html_commit_range(state, (const char*)state->cursor,
                                      (const char*)YYCURSOR);
                webhtml_builder_note_error(state, "忽略结束标签内的空字符");
                continue;
            }
        */
    }
}

/* ---------------------------------------------------------------- 
   词法入口
   ---------------------------------------------------------------- */

int webhtmllex(WEBHTMLSTYPE* yylval, struct WebHtmlParseState* state) {
    (void)yylval;
    if (state == NULL) {
        return 0;
    }
    webhtml_token_reset(state);
    if (state->failed) {
        return HTML_TOK_EOF;
    }

    if (state->raw_text_tag[0] != '\0') {
        int raw_token = web_html_scan_raw_text(state);
        if (raw_token != WEB_HTML_RETRY) {
            return raw_token;
        }
    }

    for (;;) {
        int token = WEB_HTML_RETRY;
        if (state->cursor >= state->limit || state->failed) {
            return HTML_TOK_EOF;
        }
        if (state->in_end_tag) {
            token = web_html_scan_end_tag(state);
        } else if (state->in_tag) {
            token = web_html_scan_tag(state);
        } else {
            token = web_html_scan_normal(state);
        }
        if (token != WEB_HTML_RETRY) {
            return token;
        }
    }
}
