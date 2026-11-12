/* CSS 语法分析器定义，GNU Bison 3.8（C 骨架）。

   覆盖渲染所需的 CSS 子集：
   - 样式表 → 规则列表；
   - 规则 → 选择器组 + 声明块；
   - 选择器 → 复合选择器链（类型/类/ID/属性/伪类/通配；
     后代/子代/相邻兄弟/通用兄弟组合子）；
   - 声明 → 属性 + 值 + 可选 !important；
   - 未知 at-rule（@media、@font-face、@keyframes 等）整块跳过，
     内部花括号按平衡方式跳过。

   语义值用于构造 AST：文本由词法器分配，节点由 css_builder.h 中的桥接
   函数创建；所有接收 char* 的桥接函数接管文本所有权。

   选择器链与复合选择器采用右递归，使后续记号的决定不依赖词法空白，
   避免产生移进/归约冲突；空白本身作为 WS 记号出现在选择器上下文中。 */

%define api.prefix {webcss}
%define api.pure full
%define api.token.prefix {CSS_TOK_}
%define parse.error verbose
%parse-param { struct WebCssParseState* state }
%lex-param { struct WebCssParseState* state }

%code requires {
struct WebCssParseState;
}

%union {
    char* text;
    void* node;
    double number;
    int integer;
}

%token EOF 0
%token <text> IDENT
%token <text> FUNCTION
%token <text> AT_KEYWORD
%token <text> HASH
%token <text> STRING
%token <text> URL
%token <text> NUMBER
%token <text> DIMENSION
%token <text> PERCENTAGE
%token <text> ATTR_OP
%token <text> DELIM
%token WS
%token IMPORTANT
%token COMMA
%token COLON
%token SEMICOLON
%token LBRACE
%token RBRACE
%token LPAREN
%token RPAREN
%token LBRACKET
%token RBRACKET
%token GREATER
%token PLUS
%token TILDE
%token STAR
%token PIPE
%token DOT
%token EQUALS

%type <node> selector
%type <node> selector_tail
%type <node> compound_selector
%type <node> compound_tail
%type <node> simple_selector
%type <node> attribute_selector
%type <node> pseudo_selector
%type <node> selector_group
%type <node> declaration
%type <node> declaration_list
%type <node> declaration_block
%type <node> value_list
%type <node> value_item
%type <node> args
%type <text> arg_item
%type <text> attr_value
%type <integer> important_opt
%type <integer> combinator
%type <integer> attr_flag

%destructor { free($$); } <text>
%destructor { webcss_node_free($$); } <node>

%{
#include <stdio.h>
#include <stdlib.h>

#include "css_parser.h"
#include "css_lexer.h"
#include "css_builder.h"
%}

%start stylesheet

%%

stylesheet
    : rule_list
    ;

rule_list
    : %empty
    | rule_list ws_opt rule
    | rule_list ws_opt at_rule
    ;

ws_opt
    : %empty
    | WS
    ;

rule
    : selector_group LBRACE declaration_block RBRACE
        { webcss_emit_rule(state, $1, $3); }
    | selector_group LBRACE error RBRACE
        { webcss_node_free($1);
          webcss_note_error(state, "规则中的声明块无法解析，已跳过该规则");
          yyerrok; }
    | selector_group error
        { webcss_node_free($1);
          webcss_note_error(state, "规则语法有误，已跳过");
          yyerrok; }
    ;

selector_group
    : selector
        { $$ = webcss_selector_list_create($1); }
    | selector_group COMMA ws_opt selector
        { $$ = webcss_selector_list_append($1, $4); }
    ;

selector
    : compound_selector selector_tail
        { $$ = webcss_selector_create($1, $2); }
    ;

/* 右递归的后续环节，保证链中组合子的顺序与源文本一致。 */
selector_tail
    : %empty
        { $$ = webcss_steps_create(); }
    | combinator compound_selector selector_tail
        { $$ = webcss_steps_prepend($1, $2, $3); }
    ;

combinator
    : WS
        { $$ = WEBCSS_COMBINATOR_DESCENDANT; }
    | ws_opt GREATER ws_opt
        { $$ = WEBCSS_COMBINATOR_CHILD; }
    | ws_opt PLUS ws_opt
        { $$ = WEBCSS_COMBINATOR_NEXT_SIBLING; }
    | ws_opt TILDE ws_opt
        { $$ = WEBCSS_COMBINATOR_SUBSEQUENT_SIBLING; }
    ;

compound_selector
    : simple_selector compound_tail
        { $$ = webcss_compound_create($1, $2); }
    ;

/* 右递归的简单选择器序列；前置插入保持书写顺序。 */
compound_tail
    : %empty
        { $$ = webcss_simple_list_create(); }
    | simple_selector compound_tail
        { $$ = webcss_simple_list_prepend($1, $2); }
    ;

simple_selector
    : IDENT
        { $$ = webcss_simple_create(WEBCSS_SIMPLE_TYPE, $1, NULL, 0); }
    | STAR
        { $$ = webcss_simple_create(WEBCSS_SIMPLE_UNIVERSAL, NULL, NULL, 0); }
    | DOT IDENT
        { $$ = webcss_simple_create(WEBCSS_SIMPLE_CLASS, NULL, $2, 0); }
    | HASH
        { $$ = webcss_simple_create(WEBCSS_SIMPLE_ID, NULL, $1, 0); }
    | attribute_selector
    | pseudo_selector
    ;

attribute_selector
    : LBRACKET IDENT RBRACKET
        { $$ = webcss_attribute_create(NULL, $2, NULL, 0); }
    | LBRACKET IDENT EQUALS attr_value RBRACKET
        { $$ = webcss_attribute_create(webcss_text_create("="), $2, $4, 0); }
    | LBRACKET IDENT ATTR_OP attr_value RBRACKET
        { $$ = webcss_attribute_create($3, $2, $4, 0); }
    | LBRACKET IDENT EQUALS attr_value attr_flag RBRACKET
        { $$ = webcss_attribute_create(webcss_text_create("="), $2, $4, $5); }
    | LBRACKET IDENT ATTR_OP attr_value attr_flag RBRACKET
        { $$ = webcss_attribute_create($3, $2, $4, $5); }
    ;

attr_value
    : IDENT
        { $$ = $1; }
    | STRING
        { $$ = $1; }
    ;

attr_flag
    : IDENT
        { $$ = webcss_attr_flag($1); }
    ;

pseudo_selector
    : COLON IDENT
        { $$ = webcss_pseudo_create($2, NULL); }
    | COLON FUNCTION args RPAREN
        { $$ = webcss_pseudo_create($2, $3); }
    /* 双冒号伪元素（::before、::selection、::-webkit-scrollbar 等）。
       当前不生成伪元素盒，按未知伪类处理即可保证整份样式表继续解析。 */
    | COLON COLON IDENT
        { $$ = webcss_pseudo_create($3, NULL); }
    ;

/* 伪类实参的原文片段，拼接后交由 C++ 侧解释（如 An+B 与 :not 的简单选择器）。 */
args
    : arg_item
        { $$ = webcss_args_append(webcss_args_create(), $1); }
    | args arg_item
        { $$ = webcss_args_append($1, $2); }
    ;

arg_item
    : IDENT
        { $$ = $1; }
    | HASH
        { $$ = $1; }
    | STRING
        { $$ = webcss_text_concat_quoted($1); }
    | NUMBER
        { $$ = $1; }
    | DIMENSION
        { $$ = $1; }
    | PERCENTAGE
        { $$ = $1; }
    | URL
        { $$ = $1; }
    | DELIM
        { $$ = $1; }
    | ATTR_OP
        { $$ = $1; }
    | DOT
        { $$ = webcss_text_create("."); }
    | STAR
        { $$ = webcss_text_create("*"); }
    | COLON
        { $$ = webcss_text_create(":"); }
    | LBRACKET
        { $$ = webcss_text_create("["); }
    | RBRACKET
        { $$ = webcss_text_create("]"); }
    | EQUALS
        { $$ = webcss_text_create("="); }
    | PLUS
        { $$ = webcss_text_create("+"); }
    | GREATER
        { $$ = webcss_text_create(">"); }
    | TILDE
        { $$ = webcss_text_create("~"); }
    | PIPE
        { $$ = webcss_text_create("|"); }
    | COMMA
        { $$ = webcss_text_create(","); }
    | FUNCTION args RPAREN
        { $$ = webcss_args_function_text($1, $2); }
    ;

declaration_block
    : %empty
        { $$ = webcss_declaration_list_create(); }
    | declaration_list
        { $$ = $1; }
    ;

declaration_list
    : declaration
        { $$ = webcss_declaration_list_append(webcss_declaration_list_create(), $1); }
    | declaration_list SEMICOLON declaration
        { $$ = webcss_declaration_list_append($1, $3); }
    | declaration_list SEMICOLON
        { $$ = $1; }
    | declaration_list error SEMICOLON
        { webcss_note_error(state, "声明书写有误，已跳过该声明");
          yyerrok;
          $$ = $1; }
    ;

declaration
    : IDENT COLON value_list important_opt
        { $$ = webcss_declaration_create(state, $1, $3, $4); }
    ;

important_opt
    : %empty
        { $$ = 0; }
    | IMPORTANT
        { $$ = 1; }
    ;

value_list
    : value_item
        { $$ = webcss_value_list_append(webcss_value_list_create(), $1); }
    | value_list value_item
        { $$ = webcss_value_list_append($1, $2); }
    ;

value_item
    : IDENT
        { $$ = webcss_value_item(WEBCSS_VALUE_IDENT, $1); }
    | STRING
        { $$ = webcss_value_item(WEBCSS_VALUE_STRING, $1); }
    | HASH
        { $$ = webcss_value_item(WEBCSS_VALUE_HASH, $1); }
    | URL
        { $$ = webcss_value_item(WEBCSS_VALUE_URL, $1); }
    | NUMBER
        { $$ = webcss_value_item(WEBCSS_VALUE_NUMBER, $1); }
    | DIMENSION
        { $$ = webcss_value_item(WEBCSS_VALUE_NUMBER, $1); }
    | PERCENTAGE
        { $$ = webcss_value_item(WEBCSS_VALUE_NUMBER, $1); }
    | DELIM
        { $$ = webcss_value_item(WEBCSS_VALUE_DELIM, $1); }
    /* calc() 等函数内的运算符：当前不做算术求值，但必须能读完，
       否则整条声明与后续样式都会被跳过。 */
    | PLUS
        { $$ = webcss_value_item(WEBCSS_VALUE_DELIM, webcss_text_create("+")); }
    | STAR
        { $$ = webcss_value_item(WEBCSS_VALUE_DELIM, webcss_text_create("*")); }
    | COMMA
        { $$ = webcss_value_item(WEBCSS_VALUE_DELIM, webcss_text_create(",")); }
    | FUNCTION value_list RPAREN
        { $$ = webcss_value_function($1, $2); }
    ;

/* ----------------------------------------------------------------
   未知 at-rule：跳过整块（含嵌套花括号的平衡跳过）
   ---------------------------------------------------------------- */

at_rule
    : AT_KEYWORD at_prelude SEMICOLON
        { webcss_note_at_rule(state, $1, 0); }
    | AT_KEYWORD at_prelude LBRACE at_block RBRACE
        { webcss_note_at_rule(state, $1, 1); }
    ;

at_prelude
    : %empty
    | at_prelude prelude_token
    ;

prelude_token
    : IDENT
    | FUNCTION
    | AT_KEYWORD
    | HASH
    | STRING
    | URL
    | NUMBER
    | DIMENSION
    | PERCENTAGE
    | ATTR_OP
    | DELIM
    | WS
    | COMMA
    | COLON
    | LPAREN
    | RPAREN
    | LBRACKET
    | RBRACKET
    | GREATER
    | PLUS
    | TILDE
    | STAR
    | PIPE
    | DOT
    | EQUALS
    ;

at_block
    : %empty
    | at_block block_token
    | at_block LBRACE at_block RBRACE
    ;

block_token
    : IDENT
    | FUNCTION
    | AT_KEYWORD
    | HASH
    | STRING
    | URL
    | NUMBER
    | DIMENSION
    | PERCENTAGE
    | ATTR_OP
    | DELIM
    | IMPORTANT
    | WS
    | COMMA
    | COLON
    | SEMICOLON
    | LPAREN
    | RPAREN
    | LBRACKET
    | RBRACKET
    | GREATER
    | PLUS
    | TILDE
    | STAR
    | PIPE
    | DOT
    | EQUALS
    ;

%%

/* 语法错误：把 bison 的诊断信息连同当前记号位置转交解析状态。
   解析不在此中断，后续规则由 declaration_list 与 rule 的 error 规则恢复。 */
void webcsserror(struct WebCssParseState* state, const char* message) {
    char buffer[320];
    if (state == NULL) {
        return;
    }
    snprintf(buffer, sizeof(buffer), "第 %d 行第 %d 列附近：%s", state->token.line,
             state->token.column, message != NULL ? message : "语法错误");
    webcss_note_error(state, buffer);
}
