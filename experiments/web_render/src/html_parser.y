/* HTML 语法分析器定义，GNU Bison 3.8（C 骨架）。
   语义值只作占位（%union 中的 unused 字段），真实记号负载位于
   WebHtmlParseState::token 与待提交标签信息中，语法动作通过
   html_builder.h 声明的桥接函数向构建器提交内容。 */

%define api.prefix {webhtml}
%define api.pure full
%define api.token.prefix {HTML_TOK_}
%define parse.error verbose
%parse-param { struct WebHtmlParseState* state }
%lex-param { struct WebHtmlParseState* state }

%code requires {
struct WebHtmlParseState;
}

%union {
    int unused; /* 占位字段，语法动作不读取语义值 */
}

/* 记号常量在生成的 html_parser.h 中为 HTML_TOK_<名称>。
   记号的字面别名保持 ASCII：bison 4.x 之前的 Windows 版本会把非 ASCII
   别名按单字节转义写入 yytname，导致生成文件出现非法转义序列。 */
%token EOF 0
%token TEXT
%token START_TAG_OPEN
%token ATTR
%token TAG_CLOSE
%token SELF_CLOSE
%token END_TAG_OPEN
%token COMMENT
%token DOCTYPE
%token BAD

%start document

%{
#include "html_parser.h"
#include "html_lexer.h"
#include "html_builder.h"
%}

%%

document
    : content_list
        { webhtml_builder_finish(state); }
    ;

content_list
    : %empty
    | content_list content
    ;

content
    : TEXT
        { webhtml_builder_text(state, state->token.value); }
    | COMMENT
        { webhtml_builder_comment(state, state->token.value); }
    | DOCTYPE
        { webhtml_builder_doctype(state, state->token.value); }
    | start_tag
    | end_tag
    | BAD
        { webhtml_builder_note_error(state, "忽略无法识别的记号"); }
    | error
        { webhtml_builder_note_error(state, "语法错误：忽略意外记号"); yyerrok; }
    ;

start_tag
    : START_TAG_OPEN attribute_list tag_end
        { webhtml_builder_begin_tag(state,
                                    (const char* const*)state->attribute_names,
                                    (const char* const*)state->attribute_values,
                                    state->attribute_count,
                                    state->self_closing); }
    | START_TAG_OPEN error tag_end
        { webhtml_builder_note_error(state, "开始标签书写有误，按可解析部分收尾");
          webhtml_builder_begin_tag(state,
                                    (const char* const*)state->attribute_names,
                                    (const char* const*)state->attribute_values,
                                    state->attribute_count,
                                    state->self_closing);
          yyerrok; }
    ;

attribute_list
    : %empty
    | attribute_list attribute
    | attribute_list BAD
        { webhtml_builder_note_error(state, "忽略开始标签内的无效片段"); }
    ;

attribute
    : ATTR
        { webhtml_pending_add(state, &state->token.name, &state->token.value); }
    ;

tag_end
    : TAG_CLOSE
        { state->self_closing = 0; }
    | SELF_CLOSE
        { state->self_closing = 1; }
    ;

end_tag
    : END_TAG_OPEN TAG_CLOSE
        { webhtml_builder_end_tag(state, state->end_tag_name); }
    ;

%%

/* 语法错误：把 bison 的诊断信息转交构建器，由入口决定是否上报。
   不在此处中断解析，构建器的容错规则会继续处理后续内容。 */
void webhtmlerror(struct WebHtmlParseState* state, const char* message) {
    webhtml_builder_note_error(state, message);
}
