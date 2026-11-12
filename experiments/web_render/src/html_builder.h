#ifndef WEB_HTML_BUILDER_H
#define WEB_HTML_BUILDER_H

/* HTML 构建器：把语法分析得到的记号序列转换成 DOM 节点。
   本头文件只使用 C 语法，会被 html_lexer.re 与 html_parser.y 包含；
   C++ 实现在 html_builder.cpp 中。 */

#ifdef __cplusplus
extern "C" {
#endif

struct WebHtmlBuilder;
struct WebHtmlParseState;

/* 创建构建器。document 为 web::Document*；parent 为 web::Element*，
   为 NULL 时按整文档解析（自动建立 html/head/body），
   非 NULL 时按片段解析（生成的节点追加到该元素下）。 */
struct WebHtmlBuilder* webhtml_builder_create(void* document, void* parent);
void webhtml_builder_destroy(struct WebHtmlBuilder* builder);

/* 开始标签：names/values 为等长的属性名/值数组，count 为属性个数；
   标签名与属性值均已在词法阶段小写化或实体解码。 */
void webhtml_builder_begin_tag(struct WebHtmlParseState* state,
                               const char* const* names,
                               const char* const* values,
                               int count,
                               int self_closing);

/* 结束标签；tag_name 为小写标签名，栈中不存在同名元素时忽略。 */
void webhtml_builder_end_tag(struct WebHtmlParseState* state, const char* tag_name);

/* 文本、注释与 DOCTYPE；text 已在词法阶段完成实体解码。 */
void webhtml_builder_text(struct WebHtmlParseState* state, const char* text);
void webhtml_builder_comment(struct WebHtmlParseState* state, const char* text);
void webhtml_builder_doctype(struct WebHtmlParseState* state, const char* text);

/* 记录可恢复错误（不会中断解析），并递增状态中的错误计数。 */
void webhtml_builder_note_error(struct WebHtmlParseState* state, const char* message);

/* 解析收尾：释放剩余词法资源，标记构建完成。 */
void webhtml_builder_finish(struct WebHtmlParseState* state);

/* 供 C++ 入口查询结果。 */
int webhtml_builder_failed(const struct WebHtmlBuilder* builder);
int webhtml_builder_error_count(const struct WebHtmlBuilder* builder);
const char* webhtml_builder_error_message(const struct WebHtmlBuilder* builder);

#ifdef __cplusplus
}
#endif

#endif /* WEB_HTML_BUILDER_H */
