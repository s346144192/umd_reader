#ifndef WEB_CSS_BUILDER_H
#define WEB_CSS_BUILDER_H

/* CSS 构建器桥接接口：把语法分析得到的记号序列转换成
   web::StyleSheet / Rule / Selector / Declaration。
   本头文件只使用 C 语法，会被 css_parser.y 包含；C++ 实现在 css_support.cpp 中。

   所有接收 char* 参数的函数都会接管该字符串的所有权（可以为 NULL），
   语法动作无需再释放这些文本。 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct WebCssParseState;

/* 简单选择器种类，取值与 web::SimpleSelector::Kind 一致。 */
enum {
    WEBCSS_SIMPLE_UNIVERSAL = 0,
    WEBCSS_SIMPLE_TYPE = 1,
    WEBCSS_SIMPLE_CLASS = 2,
    WEBCSS_SIMPLE_ID = 3,
    WEBCSS_SIMPLE_ATTRIBUTE = 4,
    WEBCSS_SIMPLE_PSEUDO_CLASS = 5,
};

/* 组合子，取值与 web::Combinator 一致。 */
enum {
    WEBCSS_COMBINATOR_DESCENDANT = 0,
    WEBCSS_COMBINATOR_CHILD = 1,
    WEBCSS_COMBINATOR_NEXT_SIBLING = 2,
    WEBCSS_COMBINATOR_SUBSEQUENT_SIBLING = 3,
};

/* 值分量的原始种类。 */
enum {
    WEBCSS_VALUE_IDENT = 0,
    WEBCSS_VALUE_STRING = 1,
    WEBCSS_VALUE_HASH = 2,
    WEBCSS_VALUE_NUMBER = 3,
    WEBCSS_VALUE_URL = 4,
    WEBCSS_VALUE_DELIM = 5,
    WEBCSS_VALUE_FUNCTION = 6,
};

/* 释放节点树。NULL 可安全传入；解析失败与丢弃规则时由语法动作调用。 */
void webcss_node_free(void* node);

/* 分配一份文本副本，供语法动作在无文本负载的记号处构造负载。 */
char* webcss_text_create(const char* text);

/* 把字符串内容包成带双引号的文本（内部双引号与反斜杠转义），供伪类实参回读。 */
char* webcss_text_concat_quoted(char* text);

/* 把属性选择器的 i/s 标志文本转成标志位（"i" 或 "I" 为 1），并释放 text。 */
int webcss_attr_flag(char* text);

/* 复合选择器：simple_selector 为第一个简单选择器，tail 为 webcss_simple_list_* 形成的后续序列。 */
void* webcss_compound_create(void* simple_selector, void* tail);

/* 简单选择器序列（右递归）：空尾与前置插入。 */
void* webcss_simple_list_create(void);
void* webcss_simple_list_prepend(void* simple_selector, void* tail);

/* 简单选择器。kind 为 WEBCSS_SIMPLE_*，name/value 可为 NULL。 */
void* webcss_simple_create(int kind, char* name, char* value, int case_insensitive);

/* 属性选择器。operator_text 为 "=" "~=" "|=" "^=" "$=" "*="，为 NULL 表示只判断属性存在。
   属性名在 C++ 侧小写化；属性值与 i 标志按原样保存。 */
void* webcss_attribute_create(char* operator_text, char* name, char* value, int case_insensitive);

/* 伪类选择器。args 为 webcss_args_create 返回的实参文本集合，可为 NULL。 */
void* webcss_pseudo_create(char* name, void* args);

/* 伪类实参文本集合；append 接管 text 的所有权。 */
void* webcss_args_create(void);
void* webcss_args_append(void* args, char* text);

/* 把嵌套函数实参拼成 "name(args)" 文本，返回新分配的字符串（消费 name 与 args）。 */
char* webcss_args_function_text(char* name, void* args);

/* 选择器链：compound 为首个复合选择器，steps 为 webcss_steps_* 形成的后续环节。 */
void* webcss_selector_create(void* compound, void* steps);

/* 选择器链后续环节（右递归）：空链尾与前置插入。combinator 为 WEBCSS_COMBINATOR_*。 */
void* webcss_steps_create(void);
void* webcss_steps_prepend(int combinator, void* compound, void* tail);

/* 选择器列表（逗号分隔）。 */
void* webcss_selector_list_create(void* selector);
void* webcss_selector_list_append(void* list, void* selector);

/* 值分量。kind 为 WEBCSS_VALUE_*，text 为记号原文（数字含单位）。 */
void* webcss_value_item(int kind, char* text);

/* 值列表；函数值由 name 与实参值列表拼成 "name(args)"。 */
void* webcss_value_list_create(void);
void* webcss_value_list_append(void* list, void* item);
void* webcss_value_function(char* name, void* args);

/* 声明；property 与 value_list 的所有权转移给节点。value_list 可为 NULL（空值）。 */
void* webcss_declaration_create(struct WebCssParseState* state, char* property, void* value_list,
                                int important);

/* 声明列表。 */
void* webcss_declaration_list_create(void);
void* webcss_declaration_list_append(void* list, void* declaration);

/* 把一条规则（选择器列表 + 声明列表）写入 state->stylesheet，并释放两个节点。 */
void webcss_emit_rule(struct WebCssParseState* state, void* selector_list, void* declaration_list);

/* 记录被跳过的 at-rule（@media、@font-face、@keyframes 等），消耗 name 并递增计数。 */
void webcss_note_at_rule(struct WebCssParseState* state, char* name, int has_block);

#ifdef __cplusplus
}
#endif

#endif /* WEB_CSS_BUILDER_H */
