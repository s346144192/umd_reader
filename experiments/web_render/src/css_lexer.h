#ifndef WEB_CSS_LEXER_H
#define WEB_CSS_LEXER_H

/* CSS 词法与语法分析共享的上下文定义。
   本头文件只使用 C 语法，会被 css_lexer.re 与 css_parser.y 同时包含；
   C++ 侧（css_support.cpp）也包含它来初始化解析状态。 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct WebCssParseState;

/* 由 bison 生成的 css_parser.h 定义的语义值类型。若调用方尚未包含该头文件，
   这里给出前置声明。语义值承载 AST 节点与文本负载。 */
#if !defined(WEBCSSSTYPE_IS_DECLARED)
union WEBCSSSTYPE;
typedef union WEBCSSSTYPE WEBCSSSTYPE;
#endif

/* 当前记号在源文本中的位置与负载描述。text 与语义值共用同一块内存，
   生命周期由词法器转交语法动作管理，解析状态只作只读引用。 */
struct WebCssToken {
    int kind;          /* 记号编号，与 bison 生成的 CSS_TOK_* 一致；0 表示输入结束 */
    const char* text;  /* 文本负载；无文本负载的记号为 NULL */
    size_t begin;      /* 记号起始字节偏移 */
    size_t end;        /* 记号结束字节偏移（不含） */
    int line;          /* 记号起始行号，从 1 开始 */
    int column;        /* 记号起始列号，从 1 开始 */
};

/* 词法/语法分析上下文。
   词法器维护扫描指针、行列位置与空白待发状态；语法动作通过 css_builder.h
   中的桥接函数把节点写入 stylesheet 指向的 web::StyleSheet。 */
struct WebCssParseState {
    /* 输入缓冲（尾部带哨兵字节）；limit 指向真实输入末尾之后的第一个字节。 */
    const char* input;
    const char* cursor;
    const char* limit;

    int line;    /* 当前行号，从 1 开始 */
    int column;  /* 当前列号，从 1 开始 */

    /* 花括号、圆括号、方括号嵌套深度。只在三者均为 0 时发出 WS 记号，
       使声明块与函数/属性实参内部的空白不参与语法。 */
    int brace_depth;
    int paren_depth;
    int bracket_depth;

    /* 已跳过空白或注释、待在最外层发出的 WS 记号位置。 */
    int space_pending;
    size_t space_begin;
    size_t space_end;
    int space_line;
    int space_column;

    struct WebCssToken token; /* 最近一个记号的负载与位置 */

    void* stylesheet;    /* web::StyleSheet*；规则提交目标，由 C++ 侧传入 */
    int error_count;     /* 可恢复错误的累计数量 */
    int failed;          /* 不可恢复错误（内存分配失败等） */
    int error_line;      /* 首个错误的行号 */
    int error_column;    /* 首个错误的列号 */
    size_t skipped_at_rules; /* 已跳过的 at-rule 数量 */
    char error_message[320]; /* 首个错误的消息缓冲 */
};

/* 词法入口。签名与 bison 生成的纯解析器调用方式一致：
   webcssparse 以 state 作为唯一参数，内部调用 webcsslex(&yylval, state)。 */
int webcsslex(WEBCSSSTYPE* yylval, struct WebCssParseState* state);

/* 由语法文件尾声实现：把 bison 报告的语法错误转交状态记录。 */
void webcsserror(struct WebCssParseState* state, const char* message);

/* 记录一条可恢复错误（只保留首条消息），递增错误计数。 */
void webcss_note_error(struct WebCssParseState* state, const char* message);

/* 复位词法资源与位置状态，供入口在解析结束后调用。 */
void webcss_state_reset(struct WebCssParseState* state);

#ifdef __cplusplus
}
#endif

#endif /* WEB_CSS_LEXER_H */
