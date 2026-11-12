/* CSS 词法分析器定义，re2c 4.3 规则，生成结果为 C 源文件。
   只识别渲染所需的 CSS 记号子集：标识、函数、at 关键字、hash、字符串、
   url()、数字/量纲/百分比、分隔符、!important、空白与注释。

   空白与注释由 web_css_skip_trivia 统一跳过：仅当花括号、圆括号、方括号
   深度均为 0，且后续有效字节确实是选择器或 at-rule 的开头时，才登记一个
   待发的 WS 记号。这样选择器中的后代组合子能被语法识别，而规则体前的
   空白、逗号前的空白以及声明块内部的空白都不会进入语法。 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "css_parser.h"
#include "css_lexer.h"
#include "css_builder.h"

/* ----------------------------------------------------------------
   基础字符与文本工具
   ---------------------------------------------------------------- */

static int web_css_is_space(int character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
           character == '\f';
}

static int web_css_lower_char(int character) {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A' + 'a';
    }
    return character;
}

/* 复制 [begin, end) 区间为以 NUL 结尾的新字符串。 */
static char* web_css_strdup_range(const char* begin, const char* end) {
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

/* 复制区间并转成 ASCII 小写。 */
static char* web_css_strdup_lower(const char* begin, const char* end) {
    size_t length = (size_t)(end - begin);
    char* text = (char*)malloc(length + 1);
    if (text == NULL) {
        return NULL;
    }
    size_t index = 0;
    for (; index < length; ++index) {
        text[index] = (char)web_css_lower_char((unsigned char)begin[index]);
    }
    text[length] = '\0';
    return text;
}

/* 复制区间并处理反斜杠转义：反斜杠只转义紧随其后的一个字节。
   十六进制码位转义不在此展开（最低限度实现）。 */
static char* web_css_unescape_range(const char* begin, const char* end) {
    size_t length = (size_t)(end - begin);
    char* text = (char*)malloc(length + 1);
    if (text == NULL) {
        return NULL;
    }
    size_t output = 0;
    const char* cursor = begin;
    while (cursor < end) {
        if (*cursor == '\\' && cursor + 1 < end) {
            text[output] = *(cursor + 1);
            output += 1;
            cursor += 2;
            continue;
        }
        text[output] = *cursor;
        output += 1;
        cursor += 1;
    }
    text[output] = '\0';
    return text;
}

/* ----------------------------------------------------------------
   空白与注释
   ---------------------------------------------------------------- */

static int web_css_trivia_start(const char* cursor, const char* limit) {
    if (cursor >= limit) {
        return 0;
    }
    if (web_css_is_space((unsigned char)*cursor)) {
        return 1;
    }
    if (cursor + 1 < limit && cursor[0] == '/' && cursor[1] == '*') {
        return 1;
    }
    return 0;
}

/* 跳过一段注释，closed 置为是否见到闭合标记（星号紧跟斜杠）。 */
static const char* web_css_skip_comment(const char* cursor, const char* limit, int* closed) {
    const char* scan = cursor + 2;
    while (scan + 1 < limit) {
        if (scan[0] == '*' && scan[1] == '/') {
            *closed = 1;
            return scan + 2;
        }
        ++scan;
    }
    *closed = 0;
    return limit;
}

/* 连续跳过空白与注释，返回下一个有效字节位置。 */
static const char* web_css_skip_trivia(const char* cursor, const char* limit, int* closed) {
    *closed = 1;
    while (cursor < limit) {
        if (web_css_is_space((unsigned char)*cursor)) {
            ++cursor;
            continue;
        }
        if (cursor + 1 < limit && cursor[0] == '/' && cursor[1] == '*') {
            int comment_closed = 1;
            cursor = web_css_skip_comment(cursor, limit, &comment_closed);
            if (!comment_closed) {
                *closed = 0;
                return cursor;
            }
            continue;
        }
        break;
    }
    return cursor;
}

/* ----------------------------------------------------------------
   位置与记号提交
   ---------------------------------------------------------------- */

/* 按消耗的字节推进行号与列号。 */
static void web_css_update_position(struct WebCssParseState* state, const char* begin,
                                    const char* end) {
    const char* cursor = begin;
    for (; cursor < end; ++cursor) {
        if (*cursor == '\n') {
            state->line += 1;
            state->column = 1;
        } else {
            state->column += 1;
        }
    }
}

/* 记录记号区间与起始位置，推进扫描指针。 */
static void web_css_commit(struct WebCssParseState* state, const char* end) {
    const char* begin = state->cursor;
    state->token.begin = (size_t)(begin - state->input);
    state->token.end = (size_t)(end - state->input);
    state->token.line = state->line;
    state->token.column = state->column;
    web_css_update_position(state, begin, end);
    state->cursor = end;
}

/* 完成一个记号。text_required 为真时分配失败视为不可恢复错误。 */
static int web_css_finish(struct WebCssParseState* state, WEBCSSSTYPE* yylval, int kind, char* text,
                          const char* end, int text_required) {
    if (text_required && text == NULL) {
        state->failed = 1;
        snprintf(state->error_message, sizeof(state->error_message), "第 %d 行：内存分配失败",
                 state->line);
        return CSS_TOK_EOF;
    }
    web_css_commit(state, end);
    state->token.kind = kind;
    state->token.text = text;
    if (yylval != NULL) {
        yylval->text = text;
    }
    return kind;
}

/* 记录词法阶段的可恢复错误（只保留首条消息）。 */
static void web_css_note(struct WebCssParseState* state, int line, int column,
                         const char* message) {
    char buffer[320];
    if (state->error_count == 0 && state->error_message[0] == '\0') {
        snprintf(buffer, sizeof(buffer), "第 %d 行第 %d 列：%s", line, column, message);
        memcpy(state->error_message, buffer, sizeof(state->error_message));
        state->error_line = line;
        state->error_column = column;
    }
    state->error_count += 1;
}

void webcss_state_reset(struct WebCssParseState* state) {
    if (state == NULL) {
        return;
    }
    state->token.kind = CSS_TOK_EOF;
    state->token.text = NULL;
    state->token.begin = 0;
    state->token.end = 0;
    state->token.line = 0;
    state->token.column = 0;
    state->space_pending = 0;
    state->space_begin = 0;
    state->space_end = 0;
    state->space_line = 0;
    state->space_column = 0;
    state->brace_depth = 0;
    state->paren_depth = 0;
    state->bracket_depth = 0;
    state->error_count = 0;
    state->error_line = 0;
    state->error_column = 0;
    state->error_message[0] = '\0';
    state->failed = 0;
}

/* ----------------------------------------------------------------
   词法入口
   ---------------------------------------------------------------- */

int webcsslex(WEBCSSSTYPE* yylval, struct WebCssParseState* state) {
    if (state == NULL) {
        return 0;
    }
    if (yylval != NULL) {
        yylval->text = NULL;
    }
    if (state->failed) {
        return CSS_TOK_EOF;
    }

    for (;;) {
        /* 先提交待发的空白记号，保证语法看到的位置顺序与实际文本一致。 */
        if (state->space_pending) {
            state->space_pending = 0;
            state->token.kind = CSS_TOK_WS;
            state->token.text = NULL;
            state->token.begin = state->space_begin;
            state->token.end = state->space_end;
            state->token.line = state->space_line;
            state->token.column = state->space_column;
            return CSS_TOK_WS;
        }

        /* 跳过空白与注释；按后续有效字节决定是否登记 WS 记号。 */
        if (web_css_trivia_start(state->cursor, state->limit)) {
            const char* begin = state->cursor;
            int start_line = state->line;
            int start_column = state->column;
            int closed = 1;
            const char* end = web_css_skip_trivia(begin, state->limit, &closed);
            if (!closed) {
                web_css_note(state, start_line, start_column, "注释未闭合");
            }
            web_css_update_position(state, begin, end);
            state->cursor = end;
            if (state->brace_depth == 0 && state->paren_depth == 0 && state->bracket_depth == 0) {
                int next = (end < state->limit) ? (unsigned char)*end : 0;
                if (next != 0 && next != '{' && next != '}' && next != ',' && next != ';' &&
                    next != '!' && next != ')' && next != ']') {
                    state->space_pending = 1;
                    state->space_begin = (size_t)(begin - state->input);
                    state->space_end = (size_t)(end - state->input);
                    state->space_line = start_line;
                    state->space_column = start_column;
                }
            }
            continue;
        }

        if (state->cursor >= state->limit) {
            state->token.kind = CSS_TOK_EOF;
            state->token.text = NULL;
            state->token.begin = (size_t)(state->cursor - state->input);
            state->token.end = state->token.begin;
            state->token.line = state->line;
            state->token.column = state->column;
            return CSS_TOK_EOF;
        }

        {
            const unsigned char* YYCURSOR = (const unsigned char*)state->cursor;
            const unsigned char* YYMARKER;
            (void)YYMARKER;
            /*!re2c
                re2c:define:YYCTYPE = "unsigned char";
                re2c:yyfill:enable = 0;

                WSCHAR = [ \t\r\n\f];
                HEX = [0-9A-Fa-f];
                ESC = "\\" ( HEX{1,6} WSCHAR? | [^ \t\r\n\f0-9A-Fa-f\x00] );
                NMSTART = [a-zA-Z_] | [\x80-\xFF] | ESC;
                NMCHAR = [a-zA-Z0-9_-] | [\x80-\xFF] | ESC;
                IDENT = "-"? "-"? NMSTART NMCHAR*;
                NUM = ([0-9]+ ("." [0-9]+)?) | ("." [0-9]+);
                SIGNED = ("+" | "-")? NUM;
                DQSTRING = "\"" ( [^"\\\x00\n\r\f] | "\\" [^\x00] )* "\"";
                SQSTRING = "'" ( [^'\\\x00\n\r\f] | "\\" [^\x00] )* "'";
                DQOPEN = "\"" ( [^"\\\x00\n\r\f] | "\\" [^\x00] )*;
                SQOPEN = "'" ( [^'\\\x00\n\r\f] | "\\" [^\x00] )*;
                URLTOKEN = [uU] [rR] [lL] "(" [^)\x00\n\r\f]* ")";
                /* 带引号的 url()：内容可包含右括号（如内嵌 SVG 的 data URI），
                   必须整体吞掉，否则会在第一个 ")" 处截断并遗留未闭合字符串。 */
                QUOTEDURL = [uU] [rR] [lL] "(" WSCHAR* (DQSTRING | SQSTRING) WSCHAR* ")";

                [\x00] {
                    /* 输入中的空字符不属于 CSS 文本，直接跳过。 */
                    web_css_commit(state, (const char*)YYCURSOR);
                    continue;
                }

                DQSTRING {
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    char* text = web_css_unescape_range(begin + 1, end - 1);
                    return web_css_finish(state, yylval, CSS_TOK_STRING, text, end, 1);
                }

                SQSTRING {
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    char* text = web_css_unescape_range(begin + 1, end - 1);
                    return web_css_finish(state, yylval, CSS_TOK_STRING, text, end, 1);
                }

                DQOPEN {
                    /* 字符串未闭合：按已有内容提交并记录错误。 */
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    char* text = web_css_unescape_range(begin + 1, end);
                    web_css_note(state, state->token.line > 0 ? state->token.line : state->line,
                                 state->column, "字符串未闭合");
                    return web_css_finish(state, yylval, CSS_TOK_STRING, text, end, 1);
                }

                SQOPEN {
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    char* text = web_css_unescape_range(begin + 1, end);
                    web_css_note(state, state->line, state->column, "字符串未闭合");
                    return web_css_finish(state, yylval, CSS_TOK_STRING, text, end, 1);
                }

                QUOTEDURL | URLTOKEN {
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    const char* inner_begin = begin + 4;
                    const char* inner_end = end - 1;
                    while (inner_begin < inner_end && web_css_is_space((unsigned char)*inner_begin)) {
                        ++inner_begin;
                    }
                    while (inner_end > inner_begin && web_css_is_space((unsigned char)*(inner_end - 1))) {
                        --inner_end;
                    }
                    if (inner_end > inner_begin + 1 && (*inner_begin == '"' || *inner_begin == '\'') &&
                        *(inner_end - 1) == *inner_begin) {
                        ++inner_begin;
                        --inner_end;
                    }
                    char* text = web_css_unescape_range(inner_begin, inner_end);
                    return web_css_finish(state, yylval, CSS_TOK_URL, text, end, 1);
                }

                "!" WSCHAR* [iI][mM][pP][oO][rR][tT][aA][nN][tT] {
                    return web_css_finish(state, yylval, CSS_TOK_IMPORTANT, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                "~=" | "|=" | "^=" | "$=" | "*=" {
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    char* text = web_css_strdup_range(begin, end);
                    return web_css_finish(state, yylval, CSS_TOK_ATTR_OP, text, end, 1);
                }

                SIGNED IDENT {
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    char* text = web_css_strdup_range(begin, end);
                    return web_css_finish(state, yylval, CSS_TOK_DIMENSION, text, end, 1);
                }

                SIGNED "%" {
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    char* text = web_css_strdup_range(begin, end);
                    return web_css_finish(state, yylval, CSS_TOK_PERCENTAGE, text, end, 1);
                }

                SIGNED {
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    char* text = web_css_strdup_range(begin, end);
                    return web_css_finish(state, yylval, CSS_TOK_NUMBER, text, end, 1);
                }

                "#" NMCHAR+ {
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    char* text = web_css_strdup_range(begin + 1, end);
                    return web_css_finish(state, yylval, CSS_TOK_HASH, text, end, 1);
                }

                "@" IDENT {
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    char* text = web_css_strdup_lower(begin + 1, end);
                    return web_css_finish(state, yylval, CSS_TOK_AT_KEYWORD, text, end, 1);
                }

                IDENT "(" {
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    char* text = web_css_strdup_lower(begin, end - 1);
                    return web_css_finish(state, yylval, CSS_TOK_FUNCTION, text, end, 1);
                }

                IDENT {
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    char* text = web_css_unescape_range(begin, end);
                    return web_css_finish(state, yylval, CSS_TOK_IDENT, text, end, 1);
                }

                "{" {
                    state->brace_depth += 1;
                    return web_css_finish(state, yylval, CSS_TOK_LBRACE, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                "}" {
                    if (state->brace_depth > 0) {
                        state->brace_depth -= 1;
                    }
                    return web_css_finish(state, yylval, CSS_TOK_RBRACE, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                "(" {
                    state->paren_depth += 1;
                    return web_css_finish(state, yylval, CSS_TOK_LPAREN, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                ")" {
                    if (state->paren_depth > 0) {
                        state->paren_depth -= 1;
                    }
                    return web_css_finish(state, yylval, CSS_TOK_RPAREN, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                "[" {
                    state->bracket_depth += 1;
                    return web_css_finish(state, yylval, CSS_TOK_LBRACKET, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                "]" {
                    if (state->bracket_depth > 0) {
                        state->bracket_depth -= 1;
                    }
                    return web_css_finish(state, yylval, CSS_TOK_RBRACKET, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                "," {
                    return web_css_finish(state, yylval, CSS_TOK_COMMA, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                ":" {
                    return web_css_finish(state, yylval, CSS_TOK_COLON, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                ";" {
                    return web_css_finish(state, yylval, CSS_TOK_SEMICOLON, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                ">" {
                    return web_css_finish(state, yylval, CSS_TOK_GREATER, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                "+" {
                    return web_css_finish(state, yylval, CSS_TOK_PLUS, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                "~" {
                    return web_css_finish(state, yylval, CSS_TOK_TILDE, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                "*" {
                    return web_css_finish(state, yylval, CSS_TOK_STAR, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                "|" {
                    return web_css_finish(state, yylval, CSS_TOK_PIPE, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                "." {
                    return web_css_finish(state, yylval, CSS_TOK_DOT, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                "=" {
                    return web_css_finish(state, yylval, CSS_TOK_EQUALS, NULL,
                                          (const char*)YYCURSOR, 0);
                }

                * {
                    /* 其余单字节按分隔符提交，保证扫描始终前进。 */
                    const char* begin = (const char*)state->cursor;
                    const char* end = (const char*)YYCURSOR;
                    char* text = web_css_strdup_range(begin, end);
                    return web_css_finish(state, yylval, CSS_TOK_DELIM, text, end, 1);
                }
            */
        }
    }
}
