#ifndef WEB_HTML_LEXER_H
#define WEB_HTML_LEXER_H

/* HTML 词法与语法分析共享的上下文定义。
   本头文件只使用 C 语法，会被 html_lexer.re 与 html_parser.y 同时包含；
   C++ 侧（html_builder.cpp）也可以包含它来读取记号负载。 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct WebHtmlBuilder;

/* 由 bison 生成的 html_parser.h 定义的语义值类型。若调用方尚未包含该头文件，
   这里给出前置声明；语义值本身只作占位，真实负载放在 WebHtmlParseState 中。 */
#if !defined(WEBHTMLSTYPE_IS_DECLARED)
union WEBHTMLSTYPE;
typedef union WEBHTMLSTYPE WEBHTMLSTYPE;
#endif

/* 单个记号的可读负载。字符串均为 malloc 分配的以 NUL 结尾的内存，
   生命周期由 webhtml_token_reset 统一管理。 */
struct WebHtmlToken {
    char* name;   /* 标签名或属性名（小写） */
    char* value;  /* 属性值、文本、注释或 DOCTYPE 内容（实体已解码） */
    size_t begin; /* 记号在输入中的起始字节偏移 */
    size_t end;   /* 记号在输入中的结束字节偏移（不含） */
    int line;     /* 记号起始行号，从 1 开始 */
};

/* 词法/语法分析上下文。词法器维护扫描位置与模式，语法动作读取当前记号负载。 */
struct WebHtmlParseState {
    /* 输入缓冲（尾部带哨兵字节）；limit 指向真实输入末尾之后的第一个字节。 */
    const char* input;
    const char* cursor;
    const char* limit;

    int line;         /* 当前行号，从 1 开始 */
    int in_tag;       /* 正在扫描开始标签的标签体（属性列表） */
    int in_end_tag;   /* 正在扫描结束标签，只等待 '>' */
    int self_closing; /* 最近完成的开始标签是否以 "/>" 自闭合 */

    char* open_tag_name; /* 待提交的开始标签名（小写） */
    char* end_tag_name;  /* 最近一个结束标签名（小写） */

    /* 待提交开始标签的源位置：起始偏移与行号在读到标签名时确定，
       结束偏移在读到 '>' 或 "/>" 时确定，供构建器写入节点的 SourceSpan。 */
    size_t open_tag_begin;
    size_t open_tag_end;
    int open_tag_line;

    /* 处于脚本/样式原始文本模式时的标签名；为空表示普通模式。 */
    char raw_text_tag[8];
    /* 开始标签结束后需要进入原始文本模式的标签名（script/style）。 */
    char pending_raw_text_tag[8];

    /* 待提交的属性列表；names 与 values 按索引配对，字符串由词法器分配。 */
    char** attribute_names;
    char** attribute_values;
    int attribute_count;
    int attribute_capacity;

    struct WebHtmlToken token; /* 最近一个记号的负载 */
    struct WebHtmlBuilder* builder;

    int error_count; /* 可恢复错误的累计数量 */
    int failed;      /* 不可恢复错误（内存分配失败等） */
};

/* 词法入口。签名与 bison 生成的纯解析器调用方式一致。 */
int webhtmllex(WEBHTMLSTYPE* yylval, struct WebHtmlParseState* state);

/* 由语法文件的尾声实现：把 bison 报告的错误转交构建器记录。 */
void webhtmlerror(struct WebHtmlParseState* state, const char* message);

/* 生命周期管理，均由词法器实现。 */
void webhtml_token_reset(struct WebHtmlParseState* state);
void webhtml_pending_reset(struct WebHtmlParseState* state);
void webhtml_state_reset(struct WebHtmlParseState* state);

/* 把 name/value 的所有权移入待提交属性列表（成功后两个指针置空）。 */
void webhtml_pending_add(struct WebHtmlParseState* state, char** name, char** value);

/* 解码 HTML 实体引用，返回 malloc 分配的字符串；失败返回 NULL。 */
char* webhtml_decode_entities(const char* begin, const char* end);

/* 释放本模块分配的字符串，供 C++ 侧调用。 */
void webhtml_free_string(char* memory);

#ifdef __cplusplus
}
#endif

#endif /* WEB_HTML_LEXER_H */
