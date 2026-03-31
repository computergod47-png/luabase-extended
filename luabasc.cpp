
// Build: clang++ -o luabasec luabasc4.cpp $(python3-config --embed --cflags --ldflags) -O2
//
// NEW FEATURES ADDED (30):
//  F01  for-range loop           for i = start, limit [, step] do ... end
//  F02  break statement          break
//  F03  continue statement       continue
//  F04  logical NOT              not expr
//  F05  bitwise NOT              ~expr
//  F06  bitwise AND              expr & expr
//  F07  bitwise OR               expr | expr
//  F08  bitwise XOR              expr ^ expr
//  F09  shift left / right       expr << expr  /  expr >> expr
//  F10  println                  println(...)  — print + newline
//  F11  assert                   assert(cond)
//  F12  switch / case / default  switch expr { case v: ... default: ... }
//  F13  const declarations       const int N = 42
//  F14  explicit cast            cast(type, expr)
//  F15  typeof() string          typeof(var)  => compile-time type string
//  F16  sizeof()                 sizeof(var_or_type)
//  F17  ternary expression       cond ? a : b
//  F18  compound assignment      +=  -=  *=  /=  %=
//  F19  increment / decrement    ++  --  (prefix and postfix)
//  F20  power operator           expr ** expr  =>  pow(a,b)
//  F21  fread statement          fread(var)
//  F22  strlen()                 strlen(str)
//  F23  strcpy / strcat          strcpy(dst,src)  strcat(dst,src)
//  F24  memset                   memset(ptr,val,n)
//  F25  exit()                   exit(code)
//  F26  enum declaration         enum Name { A, B=5, C }
//  F27  bool type + literals     bool flag = true / false
//  F28  printfmt                 printfmt("%d\n", x)  — raw printf
//  F29  char type                char c = 'A'
//  F30  // and /**/ comments + hex literals 0x1F
//
// BUGS FIXED (all from original):
//  BUG-A / BUG-A2  __m256i branch missing from TO_STR -> wrong _lb_i path
//  BUG-B           var_types lookup used raw name instead of safe_name mangled key
//  BUG-C / BUG-C2  __m256/__m256i in fwrite used raw fprintf("%g",vec) -> compile error
//  BUG-D           float in fwrite fell through to else branch silently
//  BUG-E           scanf for short/long/double used wrong format specifiers
//  BUG-F           (covered by BUG-E fix — double required %lf not %f)
//  BUG-G           scanf for __m256/__m256i wrote garbage silently; now hard error
//  BUG-H           _handle_declared not shared with .lh; FILE* re-declared
//  BUG-I           PyLong_AsLong called on None/str causing CPython crash
//  BUG-J           No #line directives emitted (now emitted for every statement)
//  BUG-K           Struct field names not passed through safe_name()
//  BUG-L           Bare 'void' struct field not rejected (hard C error)
//  BUG-M / NOTE-4  'str name' emitted 'char* name;' (uninitialised ptr) -> char name[256]
//  NOTE-10         body_str join embedded #line in middle of line
//  NOTE-11         Windows path backslashes not escaped in #line strings
//  EXTRA           fclose now sets handle=NULL to prevent double-free
//  EXTRA           snprintf used instead of sprintf (buffer overflow safety)
//  EXTRA           NULL check + OOM guard on PyMem_RawMalloc for w_argv
//  EXTRA           return statement with no value (void functions) now parses correctly

#include <Python.h>
#include <wchar.h>

const char* transpiler_logic = R"LB(#!/usr/bin/env python3
import sys, os, subprocess

class TokenType:
    NUMBER="NUMBER"; STRING="STRING"; IDENTIFIER="IDENTIFIER"
    IF="IF"; THEN="THEN"; ELSE="ELSE"; END="END"
    WHILE="WHILE"; DO="DO"; FUNCTION="FUNCTION"; RETURN="RETURN"
    PRINT="PRINT"; SCANF="SCANF"; INT="INT"; STR="STR"; FLOAT="FLOAT"
    FOPEN="FOPEN"; FWRITE="FWRITE"; FCLOSE="FCLOSE"; HLT="HLT"
    ASSIGN="ASSIGN"; PLUS="PLUS"; MINUS="MINUS"; MULTIPLY="MULTIPLY"
    DIVIDE="DIVIDE"; EQ="EQ"; NE="NE"; LT="LT"; GT="GT"; MOD="MOD"
    LE="LE"; GE="GE"
    LPAREN="LPAREN"; RPAREN="RPAREN"; LBRACE="LBRACE"; RBRACE="RBRACE"
    COMMA="COMMA"; EOF="EOF"; LINK="LINK"; LSBRACKET="LSBRACKET"; RSBRACKET="RSBRACKET"
    LONG="LONG"; DOUBLE="DOUBLE"; M256="M256"; VOID="VOID"; SHORT="SHORT"; M256I="M256I"
    TYPE="TYPE"; ELSEIF="ELSEIF"; AND="AND"; OR="OR"; PTR="PTR"; ADDRESS_OF="ADDRESS_OF"
    DOT="DOT"
    # F01-F30
    FOR="FOR"; BREAK="BREAK"; CONTINUE="CONTINUE"; NOT="NOT"
    BITNOT="BITNOT"; BITOR="BITOR"; BITXOR="BITXOR"
    SHL="SHL"; SHR="SHR"
    PRINTLN="PRINTLN"; ASSERT="ASSERT"
    SWITCH="SWITCH"; CASE="CASE"; DEFAULT_KW="DEFAULT_KW"; COLON="COLON"
    CONST_KW="CONST_KW"; CAST="CAST"; TYPEOF="TYPEOF"; SIZEOF_KW="SIZEOF_KW"
    QUESTION="QUESTION"
    PLUS_ASSIGN="PLUS_ASSIGN"; MINUS_ASSIGN="MINUS_ASSIGN"
    MUL_ASSIGN="MUL_ASSIGN"; DIV_ASSIGN="DIV_ASSIGN"; MOD_ASSIGN="MOD_ASSIGN"
    INCR="INCR"; DECR="DECR"; POW="POW"
    FREAD="FREAD"; STRLEN_KW="STRLEN_KW"; STRCPY_KW="STRCPY_KW"; STRCAT_KW="STRCAT_KW"
    MEMSET_KW="MEMSET_KW"; EXIT_KW="EXIT_KW"; ENUM_KW="ENUM_KW"
    BOOL_KW="BOOL_KW"; TRUE_KW="TRUE_KW"; FALSE_KW="FALSE_KW"
    PRINTFMT="PRINTFMT"; CHAR_KW="CHAR_KW"; ARROW="ARROW"
    CHAR_LIT="CHAR_LIT"; SEMICOLON="SEMICOLON"


class LuaBaseError(Exception):
    def __init__(self, kind, msg, line=None, col=None, snippet=None):
        self.kind=kind; self.line=line; self.col=col; self.snippet=snippet
        loc    = f" (line {line}, col {col})" if line is not None else ""
        detail = f"\n    {snippet}" if snippet else ""
        super().__init__(f"[{kind}]{loc}: {msg}{detail}")


class Token:
    def __init__(self, ttype, value, line=None, col=None):
        self.type=ttype; self.value=value; self.line=line; self.col=col


class Lexer:
    def __init__(self, source):
        self.source=source; self.pos=0; self.line=1; self.col=1
        self.keywords = {
            'if':TokenType.IF,'then':TokenType.THEN,'else':TokenType.ELSE,
            'end':TokenType.END,'while':TokenType.WHILE,'do':TokenType.DO,
            'function':TokenType.FUNCTION,'return':TokenType.RETURN,
            'print':TokenType.PRINT,'scanf':TokenType.SCANF,
            'int':TokenType.INT,'str':TokenType.STR,'float':TokenType.FLOAT,
            'fopen':TokenType.FOPEN,'fwrite':TokenType.FWRITE,'fclose':TokenType.FCLOSE,
            'hlt':TokenType.HLT,'link':TokenType.LINK,'long':TokenType.LONG,
            'double':TokenType.DOUBLE,'__m256':TokenType.M256,'void':TokenType.VOID,
            'short':TokenType.SHORT,'__m256i':TokenType.M256I,
            'type':TokenType.TYPE,'elseif':TokenType.ELSEIF,'ptr':TokenType.PTR,
            'for':TokenType.FOR,'break':TokenType.BREAK,'continue':TokenType.CONTINUE,
            'not':TokenType.NOT,'println':TokenType.PRINTLN,'assert':TokenType.ASSERT,
            'switch':TokenType.SWITCH,'case':TokenType.CASE,'default':TokenType.DEFAULT_KW,
            'const':TokenType.CONST_KW,'cast':TokenType.CAST,'typeof':TokenType.TYPEOF,
            'sizeof':TokenType.SIZEOF_KW,'exit':TokenType.EXIT_KW,'enum':TokenType.ENUM_KW,
            'bool':TokenType.BOOL_KW,'true':TokenType.TRUE_KW,'false':TokenType.FALSE_KW,
            'printfmt':TokenType.PRINTFMT,'char':TokenType.CHAR_KW,
            'fread':TokenType.FREAD,'strlen':TokenType.STRLEN_KW,
            'strcpy':TokenType.STRCPY_KW,'strcat':TokenType.STRCAT_KW,
            'memset':TokenType.MEMSET_KW,
        }

    def _advance_char(self):
        ch = self.source[self.pos]; self.pos += 1
        if ch == '\n': self.line += 1; self.col = 1
        else:          self.col += 1
        return ch

    def _current_line_text(self):
        start = self.source.rfind('\n', 0, self.pos)
        start = 0 if start == -1 else start + 1
        end   = self.source.find('\n', self.pos)
        end   = len(self.source) if end == -1 else end
        return self.source[start:end]

    def tokenize(self):
        tokens = []
        while self.pos < len(self.source):
            char = self.source[self.pos]

            if char.isspace(): self._advance_char(); continue

            # --- # line comment ---
            if char == '#':
                while self.pos < len(self.source) and self.source[self.pos] != '\n':
                    self._advance_char()
                continue

            # F30: // single-line comment
            if char == '/' and self.pos+1 < len(self.source) and self.source[self.pos+1] == '/':
                while self.pos < len(self.source) and self.source[self.pos] != '\n':
                    self._advance_char()
                continue

            # F30: /* block comment */
            if char == '/' and self.pos+1 < len(self.source) and self.source[self.pos+1] == '*':
                self._advance_char(); self._advance_char()
                while self.pos+1 < len(self.source):
                    if self.source[self.pos] == '*' and self.source[self.pos+1] == '/':
                        self._advance_char(); self._advance_char(); break
                    self._advance_char()
                continue

            tok_line, tok_col = self.line, self.col

            # --- Identifiers and keywords ---
            if char.isalpha() or char == '_':
                start = self.pos
                while self.pos < len(self.source) and (self.source[self.pos].isalnum() or self.source[self.pos] == '_'):
                    self._advance_char()
                val = self.source[start:self.pos]
                tokens.append(Token(self.keywords.get(val, TokenType.IDENTIFIER), val, tok_line, tok_col))

            # --- Numeric literals (decimal, float, F30: hex 0x...) ---
            elif char.isdigit():
                start = self.pos; dot_seen = False
                if char == '0' and self.pos+1 < len(self.source) and self.source[self.pos+1] in 'xX':
                    self._advance_char(); self._advance_char()
                    while self.pos < len(self.source) and self.source[self.pos] in '0123456789abcdefABCDEF':
                        self._advance_char()
                else:
                    while self.pos < len(self.source) and (self.source[self.pos].isdigit() or
                            (self.source[self.pos] == '.' and not dot_seen)):
                        if self.source[self.pos] == '.': dot_seen = True
                        self._advance_char()
                tokens.append(Token(TokenType.NUMBER, self.source[start:self.pos], tok_line, tok_col))

            # --- String literals (double-quote, with escape support) ---
            elif char == '"':
                self._advance_char(); start = self.pos
                while self.pos < len(self.source) and self.source[self.pos] != '"':
                    if self.source[self.pos] == '\n':
                        raise LuaBaseError("LexError","Unterminated string literal",
                                           tok_line, tok_col, self._current_line_text())
                    if self.source[self.pos] == '\\' and self.pos+1 < len(self.source):
                        self._advance_char()
                    self._advance_char()
                if self.pos >= len(self.source):
                    raise LuaBaseError("LexError","Unterminated string literal at end of file",tok_line,tok_col)
                val = self.source[start:self.pos]; self._advance_char()
                tokens.append(Token(TokenType.STRING, val, tok_line, tok_col))

            # F29: single-quoted char literals 'A'
            elif char == "'":
                self._advance_char(); start = self.pos
                if self.pos < len(self.source) and self.source[self.pos] == '\\':
                    self._advance_char()
                if self.pos < len(self.source):
                    self._advance_char()
                val = self.source[start:self.pos]
                if self.pos < len(self.source) and self.source[self.pos] == "'":
                    self._advance_char()
                tokens.append(Token(TokenType.CHAR_LIT, val, tok_line, tok_col))

            # --- Operators ---
            else:
                double = self.source[self.pos:self.pos+2]
                ops2 = {
                    '==':TokenType.EQ,'!=':TokenType.NE,'<=':TokenType.LE,'>=':TokenType.GE,
                    '&&':TokenType.AND,'||':TokenType.OR,
                    '<<':TokenType.SHL,'>>':TokenType.SHR,
                    '+=':TokenType.PLUS_ASSIGN,'-=':TokenType.MINUS_ASSIGN,
                    '*=':TokenType.MUL_ASSIGN,'/=':TokenType.DIV_ASSIGN,'%=':TokenType.MOD_ASSIGN,
                    '++':TokenType.INCR,'--':TokenType.DECR,
                    '**':TokenType.POW,'->':TokenType.ARROW,
                }
                if double in ops2:
                    tokens.append(Token(ops2[double], double, tok_line, tok_col))
                    self._advance_char(); self._advance_char()
                else:
                    ops1 = {
                        '+':TokenType.PLUS,'-':TokenType.MINUS,'*':TokenType.MULTIPLY,
                        '/':TokenType.DIVIDE,'%':TokenType.MOD,'=':TokenType.ASSIGN,
                        '(':TokenType.LPAREN,')':TokenType.RPAREN,
                        '{':TokenType.LBRACE,'}':TokenType.RBRACE,
                        ',':TokenType.COMMA,'<':TokenType.LT,'>':TokenType.GT,
                        ']':TokenType.RSBRACKET,'[':TokenType.LSBRACKET,
                        '&':TokenType.ADDRESS_OF,'.':TokenType.DOT,
                        '~':TokenType.BITNOT,'|':TokenType.BITOR,'^':TokenType.BITXOR,
                        '?':TokenType.QUESTION,':':TokenType.COLON,';':TokenType.SEMICOLON,
                    }
                    if char in ops1:
                        tokens.append(Token(ops1[char], char, tok_line, tok_col))
                        self._advance_char()
                    else:
                        raise LuaBaseError("LexError", f"Unexpected character {char!r}",
                                           tok_line, tok_col, self._current_line_text())

        tokens.append(Token(TokenType.EOF, None, self.line, self.col))
        return tokens


def _c_path(path):
    return path.replace('\\', '\\\\')


class CTranspiler:
    def __init__(self, tokens):
        self.tokens = tokens
        self.pos    = 0
        # EXTRA: snprintf instead of sprintf prevents buffer overflows (BUG fix)
        self.headers = [
            "#include <stdio.h>\n"
            "#include <stdbool.h>\n"
            "#include <stdlib.h>\n"
            "#include <math.h>\n"
            "#include <immintrin.h>\n"
            "#include <string.h>\n"
            "#include <unistd.h>\n",

            "char _lb_buf[512];\n"
            "char _lb_buf2[512];\n"
            "static inline char* _lb_s(char* x)        { return x; }\n"
            "static inline char* _lb_cs(const char* x) { return (char*)x; }\n"
            "static inline char* _lb_f(float x)        { snprintf(_lb_buf,sizeof(_lb_buf),\"%g\",x); return _lb_buf; }\n"
            "static inline char* _lb_d(double x)       { snprintf(_lb_buf,sizeof(_lb_buf),\"%.10g\",x); return _lb_buf; }\n"
            "static inline char* _lb_i(int x)          { snprintf(_lb_buf,sizeof(_lb_buf),\"%d\",x); return _lb_buf; }\n"
            "static inline char* _lb_l(long x)         { snprintf(_lb_buf,sizeof(_lb_buf),\"%ld\",x); return _lb_buf; }\n"
            "static inline char* _lb_u(short x)        { snprintf(_lb_buf,sizeof(_lb_buf),\"%d\",(int)x); return _lb_buf; }\n"
            "static inline char* _lb_b(int x)          { return x ? \"true\" : \"false\"; }\n"
            "static inline char* _lb_c(char x)         { _lb_buf[0]=x; _lb_buf[1]='\\0'; return _lb_buf; }\n"
            "static inline char* _lb_m(__m256 v)  { float f[8]; _mm256_storeu_ps(f,v); "
                "snprintf(_lb_buf,sizeof(_lb_buf),\"[%g,%g,%g,%g,%g,%g,%g,%g]\","
                "f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7]); return _lb_buf; }\n"
            # BUG-A2 FIXED: __m256i via union (avoids alignment requirement)
            "static inline char* _lb_mi(__m256i v) { union{__m256i v;int i[8];}u; u.v=v; "
                "snprintf(_lb_buf,sizeof(_lb_buf),\"[%d,%d,%d,%d,%d,%d,%d,%d]\","
                "u.i[0],u.i[1],u.i[2],u.i[3],u.i[4],u.i[5],u.i[6],u.i[7]); return _lb_buf; }\n"
            # BUG-A FIXED: added __m256i:_lb_mi, bool:_lb_b, char:_lb_c
            "#define TO_STR(x) _Generic((x),"
                "char*:_lb_s,const char*:_lb_cs,"
                "__m256:_lb_m,__m256i:_lb_mi,"
                "float:_lb_f,double:_lb_d,"
                "int:_lb_i,long:_lb_l,short:_lb_u,"
                "bool:_lb_b,char:_lb_c,"
                "default:_lb_i)(x)\n"
        ]
        self.functions        = []
        self.main_body        = []
        self.var_types        = {}
        self._handle_declared = False
        self._source_file     = "<unknown>"
        self._enum_names      = set()

    def current(self): return self.tokens[self.pos]
    def advance(self): t = self.current(); self.pos += 1; return t

    def expect(self, ttype, shut):
        tok = self.current()
        if tok.type == ttype: return self.advance()
        got = repr(tok.value) if tok.value is not None else tok.type
        if not shut:
            raise LuaBaseError("SyntaxError", f"Expected {ttype}, got {got}", tok.line, tok.col)
        return None

    def safe_name(self, name):
        c_keywords = {
            'double','int','float','char','while','if','return','break',
            'FILE','long','void','short','__m256','__m256i',
            'for','do','else','struct','typedef','switch','case','default',
            'const','static','unsigned','signed','extern','goto','sizeof',
            'enum','union','continue','register','volatile','auto','bool','true','false'
        }
        return f"var_{name}" if name in c_keywords else name

    # BUG-J FIXED: now emits #line directives
    def _line_directive(self, tok):
        if tok is None or tok.line is None: return ""
        # NOTE-11 FIXED: escape Windows backslashes
        return f'#line {tok.line} "{_c_path(self._source_file)}"\n'

    def transpile(self, source_dir=".", source_file="<unknown>"):
        self._source_dir  = source_dir
        self._source_file = source_file
        self._lh_included = set()

        while self.current().type != TokenType.EOF:
            tok = self.current()
            if tok.type == TokenType.SEMICOLON: self.advance(); continue
            if tok.type == TokenType.TYPE:
                self.headers.append(self._line_directive(tok) + self.parse_type_definition())
            elif tok.type == TokenType.ENUM_KW:
                self.headers.append(self._line_directive(tok) + self.parse_enum())
            elif tok.type == TokenType.FUNCTION:
                self.functions.append(self._line_directive(tok) + self.parse_function())
            elif tok.type == TokenType.LINK:
                self.advance()
                ft = self.expect(TokenType.STRING, True)
                if ft is None:
                    raise LuaBaseError("SyntaxError","'link' requires a string filename")
                if ft.value.endswith(".lh"):
                    self._transpile_lh(ft.value, ft)
                else:
                    self.headers.append(f'#include "{ft.value}"')
            else:
                tok = self.current()
                stmt = self.parse_statement()
                if stmt:
                    self.main_body.append(self._line_directive(tok) + stmt)

        # NOTE-10 FIXED: each entry is self-contained, join with plain newline
        body_str = "\n    ".join(self.main_body)
        res  = "\n".join(self.headers) + "\n"
        res += "\n".join(self.functions) + "\n"
        res += "int main(int argc, char** argv) {\n    "
        res += body_str
        res += "\n    return 0;\n}"
        return res

    def _transpile_lh(self, fname, tok):
        path      = fname if os.path.isabs(fname) else os.path.join(self._source_dir, fname)
        canonical = os.path.normpath(os.path.abspath(path))
        if canonical in self._lh_included: return
        self._lh_included.add(canonical)
        if not os.path.exists(canonical):
            raise LuaBaseError("LinkError",
                f"Cannot find .lh file '{fname}' (looked in '{self._source_dir}')", tok.line, tok.col)
        try:
            with open(canonical,'r',encoding='utf-8') as f: lh_source = f.read()
        except OSError as e:
            raise LuaBaseError("LinkError",f"Could not read '{fname}': {e}",tok.line,tok.col)
        try:
            lh_tokens = Lexer(lh_source).tokenize()
        except LuaBaseError as e:
            raise LuaBaseError(e.kind,f"In '{fname}': {e}",tok.line,tok.col)

        lh = CTranspiler(lh_tokens)
        lh._source_dir   = os.path.dirname(canonical)
        lh._source_file  = canonical
        lh._lh_included  = self._lh_included
        lh.var_types     = self.var_types
        # BUG-H FIXED: share _handle_declared
        lh._handle_declared = self._handle_declared

        while lh.current().type != TokenType.EOF:
            t = lh.current()
            if t.type == TokenType.SEMICOLON: lh.advance(); continue
            if t.type == TokenType.FUNCTION:
                self.functions.append(lh._line_directive(t) + lh.parse_function())
            elif t.type == TokenType.TYPE:
                self.headers.append(lh._line_directive(t) + lh.parse_type_definition())
            elif t.type == TokenType.ENUM_KW:
                self.headers.append(lh._line_directive(t) + lh.parse_enum())
            elif t.type == TokenType.LINK:
                lh.advance()
                nt = lh.expect(TokenType.STRING, True)
                if nt is not None:
                    if nt.value.endswith(".lh"): self._transpile_lh(nt.value, nt)
                    else: self.headers.append(f'#include "{nt.value}"')
            else:
                lh.parse_statement()

        # BUG-H FIXED: propagate back
        self._handle_declared = lh._handle_declared

    # F26: enum Name { A, B=5, C }
    def parse_enum(self):
        self.advance()  # skip 'enum'
        name_tok  = self.current()
        enum_name = self.expect(TokenType.IDENTIFIER, False).value
        self._enum_names.add(enum_name)
        self.expect(TokenType.LBRACE, False)
        members = []
        while self.current().type != TokenType.RBRACE:
            if self.current().type == TokenType.EOF:
                raise LuaBaseError("SyntaxError",
                    f"Unterminated enum '{enum_name}'", name_tok.line, name_tok.col)
            m = self.expect(TokenType.IDENTIFIER, False).value
            if self.current().type == TokenType.ASSIGN:
                self.advance(); members.append(f"{m} = {self.parse_expr()}")
            else:
                members.append(m)
            if self.current().type == TokenType.COMMA: self.advance()
        self.expect(TokenType.RBRACE, False)
        return f"typedef enum {{\n    " + ",\n    ".join(members) + f"\n}} {enum_name};\n"

    def parse_type_definition(self):
        self.advance()  # skip 'type'
        name_tok    = self.current()
        struct_name = self.expect(TokenType.IDENTIFIER, False).value
        self.expect(TokenType.LBRACE, False)
        fields = []
        valid_field_types = {
            TokenType.INT,TokenType.FLOAT,TokenType.STR,TokenType.LONG,
            TokenType.SHORT,TokenType.DOUBLE,TokenType.VOID,TokenType.PTR,
            TokenType.M256,TokenType.M256I,TokenType.IDENTIFIER,
            TokenType.BOOL_KW,TokenType.CHAR_KW,
        }
        while self.current().type != TokenType.RBRACE:
            if self.current().type == TokenType.EOF:
                raise LuaBaseError("SyntaxError",
                    f"Unterminated type '{struct_name}'", name_tok.line, name_tok.col)
            ft = self.current()
            if ft.type not in valid_field_types:
                raise LuaBaseError("SyntaxError",
                    f"Expected field type in '{struct_name}', got {repr(ft.value)}", ft.line, ft.col)
            f_type_raw = self.advance().value
            if f_type_raw == "ptr":
                inner  = self.advance().value
                f_type = f"{inner.replace('str','char*')}*"
            else:
                # BUG-L FIXED: reject bare void in struct field
                if f_type_raw == "void":
                    raise LuaBaseError("SyntaxError",
                        f"'void' is invalid as a struct field in '{struct_name}'. Use 'ptr void'.",
                        ft.line, ft.col)
                f_type = f_type_raw.replace("str","char*")
            # BUG-K FIXED: safe_name for field names
            f_name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            if self.current().type == TokenType.LSBRACKET:
                self.advance()
                sz = self.expect(TokenType.NUMBER, False).value
                self.expect(TokenType.RSBRACKET, False)
                fields.append(f"{f_type} {f_name}[{sz}];")
            else:
                fields.append(f"{f_type} {f_name};")
            if self.current().type == TokenType.SEMICOLON: self.advance()
        self.expect(TokenType.RBRACE, False)
        self.var_types[struct_name] = "STRUCT"
        return f"typedef struct {{\n    " + "\n    ".join(fields) + f"\n}} {struct_name};\n"

    def parse_statement(self):
        while self.current().type == TokenType.SEMICOLON: self.advance()
        t = self.current()

        if t.type == TokenType.TYPE:    return self.parse_type_definition()
        if t.type == TokenType.ENUM_KW: return self.parse_enum()

        # HLT
        if t.type == TokenType.HLT:
            self.advance(); self.expect(TokenType.LPAREN,False)
            ms = self.parse_expr(); self.expect(TokenType.RPAREN,False)
            return f"usleep(({ms})*1000);"

        # fopen
        if t.type == TokenType.FOPEN:
            self.advance(); self.expect(TokenType.LPAREN,False)
            fname = self.parse_expr(); self.expect(TokenType.COMMA,False)
            mode  = self.parse_expr(); self.expect(TokenType.RPAREN,False)
            if not self._handle_declared:
                self._handle_declared = True
                return (f'FILE* handle = fopen({fname},{mode});\n'
                        f'    if(handle==NULL){{fprintf(stderr,"FAIL: %s\\n",{fname});exit(1);}}')
            else:
                return (f'handle = fopen({fname},{mode});\n'
                        f'    if(handle==NULL){{fprintf(stderr,"FAIL: %s\\n",{fname});exit(1);}}')

        # fwrite
        if t.type == TokenType.FWRITE:
            self.advance(); self.expect(TokenType.LPAREN,False)
            ct = self.current(); content = self.parse_expr(); self.expect(TokenType.RPAREN,False)
            # BUG-B FIXED: use safe_name for var_types lookup
            if ct.type == TokenType.IDENTIFIER:
                vt = self.var_types.get(self.safe_name(ct.value),"")
            else:
                vt = ""
            # BUG-C / BUG-D FIXED: full dispatch
            if (vt in ("str","char*")) or (vt=="" and ct.type==TokenType.STRING):
                return f'if(handle){{fprintf(handle,"%s",{content});fflush(handle);}}'
            elif vt in ("int","short"): return f'if(handle){{fprintf(handle,"%d",{content});fflush(handle);}}'
            elif vt=="char":            return f'if(handle){{fprintf(handle,"%c",{content});fflush(handle);}}'
            elif vt=="long":            return f'if(handle){{fprintf(handle,"%ld",{content});fflush(handle);}}'
            elif vt=="double":          return f'if(handle){{fprintf(handle,"%.10g",{content});fflush(handle);}}'
            elif vt=="float":           return f'if(handle){{fprintf(handle,"%g",{content});fflush(handle);}}'  # BUG-D FIXED
            elif vt in ("__m256","__m256i"): return f'if(handle){{fprintf(handle,"%s",TO_STR({content}));fflush(handle);}}'  # BUG-C FIXED
            else:                       return f'if(handle){{fprintf(handle,"%s",TO_STR({content}));fflush(handle);}}'

        # fclose — EXTRA: set handle=NULL after close to prevent double-free
        if t.type == TokenType.FCLOSE:
            self.advance(); self.expect(TokenType.LPAREN,False); self.expect(TokenType.RPAREN,False)
            return "if(handle){fclose(handle);handle=NULL;}"

        # F21: fread(var)
        if t.type == TokenType.FREAD:
            self.advance(); self.expect(TokenType.LPAREN,False)
            name = self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)
            self.expect(TokenType.RPAREN,False)
            return f"if(handle)fread(&{name},sizeof({name}),1,handle);"

        # F25: exit(code)
        if t.type == TokenType.EXIT_KW:
            self.advance(); self.expect(TokenType.LPAREN,False)
            code = self.parse_expr(); self.expect(TokenType.RPAREN,False)
            return f"exit({code});"

        # Type declarations
        is_custom = (t.type==TokenType.IDENTIFIER and
                     (self.var_types.get(t.value)=="STRUCT" or t.value in self._enum_names))
        if t.type in (TokenType.INT,TokenType.FLOAT,TokenType.STR,TokenType.PTR,
                      TokenType.LONG,TokenType.SHORT,TokenType.DOUBLE,TokenType.VOID,
                      TokenType.M256,TokenType.M256I,TokenType.BOOL_KW,TokenType.CHAR_KW) or is_custom:
            vtype_raw = self.advance().value
            if vtype_raw=="ptr":
                inner = self.advance().value
                vtype = f"{inner.replace('str','char*')}*"
            elif vtype_raw=="bool": vtype="bool"
            elif vtype_raw=="char": vtype="char"
            else: vtype=vtype_raw.replace("str","char*")
            name = self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)
            self.var_types[name] = vtype_raw
            if self.current().type==TokenType.LSBRACKET:
                self.advance(); size=self.expect(TokenType.NUMBER,False).value; self.expect(TokenType.RSBRACKET,False)
                self.var_types[name]=f"{vtype_raw}_ARRAY"; val=""
                if self.current().type==TokenType.ASSIGN:
                    self.advance(); self.expect(TokenType.LBRACE,False)
                    items=[]
                    while self.current().type!=TokenType.RBRACE:
                        items.append(self.parse_expr())
                        if self.current().type==TokenType.COMMA: self.advance()
                    self.expect(TokenType.RBRACE,False); val=f" = {{{', '.join(items)}}}"
                return f"{vtype} {name}[{size}]{val};"
            if self.current().type==TokenType.ASSIGN:
                self.advance(); val=self.parse_expr()
                return f"{vtype} {name} = {val};"
            else:
                # BUG-M FIXED: 'str name' => char name[256] not char* name (uninit ptr)
                if vtype_raw=="str":    return f"char {name}[256];"
                if vtype_raw=="char":   return f"char {name} = '\\0';"
                if vtype_raw=="bool":   return f"bool {name} = false;"
                return f"{vtype} {name};"

        # F13: const type name = val
        if t.type==TokenType.CONST_KW:
            self.advance(); inner_raw=self.advance().value
            inner_c = "char*" if inner_raw=="str" else inner_raw
            name = self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)
            self.expect(TokenType.ASSIGN,False); val=self.parse_expr()
            self.var_types[name]=inner_raw
            return f"const {inner_c} {name} = {val};"

        # print
        if t.type==TokenType.PRINT:
            self.advance(); self.expect(TokenType.LPAREN,False)
            exprs=[]
            while self.current().type!=TokenType.RPAREN:
                if self.current().type==TokenType.EOF:
                    raise LuaBaseError("SyntaxError","Unterminated 'print'",t.line,t.col)
                exprs.append(self.parse_expr())
                if self.current().type==TokenType.COMMA: self.advance()
            self.expect(TokenType.RPAREN,False)
            return "; ".join(f'printf("%s",TO_STR({e}))' for e in exprs)+";"

        # F10: println
        if t.type==TokenType.PRINTLN:
            self.advance(); self.expect(TokenType.LPAREN,False)
            exprs=[]
            while self.current().type!=TokenType.RPAREN:
                if self.current().type==TokenType.EOF:
                    raise LuaBaseError("SyntaxError","Unterminated 'println'",t.line,t.col)
                exprs.append(self.parse_expr())
                if self.current().type==TokenType.COMMA: self.advance()
            self.expect(TokenType.RPAREN,False)
            parts="; ".join(f'printf("%s",TO_STR({e}))' for e in exprs)
            return f'{parts}; printf("\\n");'

        # F28: printfmt("fmt", ...)
        if t.type==TokenType.PRINTFMT:
            self.advance(); self.expect(TokenType.LPAREN,False)
            args=[]
            while self.current().type!=TokenType.RPAREN:
                if self.current().type==TokenType.EOF:
                    raise LuaBaseError("SyntaxError","Unterminated 'printfmt'",t.line,t.col)
                args.append(self.parse_expr())
                if self.current().type==TokenType.COMMA: self.advance()
            self.expect(TokenType.RPAREN,False)
            return f'printf({", ".join(args)});'

        # F11: assert
        if t.type==TokenType.ASSERT:
            self.advance(); self.expect(TokenType.LPAREN,False)
            cond=self.parse_expr(); self.expect(TokenType.RPAREN,False)
            return (f'do{{if(!({cond}))'
                    f'{{fprintf(stderr,"[assert] FAILED line {t.line}: assert({cond})\\n");exit(1);}}'
                    f'}}while(0);')

        # if / elseif / else / end
        if t.type==TokenType.IF:
            self.advance(); cond=self.parse_expr(); self.expect(TokenType.THEN,False)
            body=[]
            while self.current().type not in (TokenType.ELSEIF,TokenType.ELSE,TokenType.END,TokenType.EOF):
                tok2=self.current(); s=self.parse_statement()
                if s: body.append(self._line_directive(tok2)+"    "+s)
            if self.current().type==TokenType.EOF:
                raise LuaBaseError("SyntaxError","Unterminated 'if'",t.line,t.col)
            elseif_parts=[]
            while self.current().type==TokenType.ELSEIF:
                ei=self.current(); self.advance(); ce=self.parse_expr(); self.expect(TokenType.THEN,False)
                be=[]
                while self.current().type not in (TokenType.ELSEIF,TokenType.ELSE,TokenType.END,TokenType.EOF):
                    tok2=self.current(); s=self.parse_statement()
                    if s: be.append(self._line_directive(tok2)+"    "+s)
                if self.current().type==TokenType.EOF:
                    raise LuaBaseError("SyntaxError","Unterminated 'elseif'",ei.line,ei.col)
                elseif_parts.append(f"}} else if ({ce}) {{\n"+"\n".join(be))
            else_part=""
            if self.current().type==TokenType.ELSE:
                self.advance(); eb=[]
                while self.current().type not in (TokenType.END,TokenType.EOF):
                    tok2=self.current(); s=self.parse_statement()
                    if s: eb.append(self._line_directive(tok2)+"    "+s)
                else_part=" else {\n"+"\n".join(eb)+"\n}"
            self.expect(TokenType.END,False)
            res = f"if ({cond}) {{\n"+"\n".join(body)+"\n"
            if elseif_parts: res+="\n".join(elseif_parts)+"\n"
            if else_part:    return res+"}"+else_part
            elif elseif_parts: return res+"}"
            else:              return res+"}"

        # while / do / end
        if t.type==TokenType.WHILE:
            self.advance(); cond=self.parse_expr(); self.expect(TokenType.DO,False)
            body=[]
            while self.current().type not in (TokenType.END,TokenType.EOF):
                tok2=self.current(); s=self.parse_statement()
                if s: body.append(self._line_directive(tok2)+s)
            if self.current().type==TokenType.EOF:
                raise LuaBaseError("SyntaxError","Unterminated 'while'",t.line,t.col)
            self.expect(TokenType.END,False)
            return f"while ({cond}) {{\n    "+"    \n".join(body)+"\n}"

        # F01: for i = start, limit [, step] do ... end
        if t.type==TokenType.FOR:
            self.advance()
            var=self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)
            self.expect(TokenType.ASSIGN,False)
            start=self.parse_expr(); self.expect(TokenType.COMMA,False)
            limit=self.parse_expr(); step="1"
            if self.current().type==TokenType.COMMA:
                self.advance(); step=self.parse_expr()
            self.expect(TokenType.DO,False)
            body=[]
            while self.current().type not in (TokenType.END,TokenType.EOF):
                tok2=self.current(); s=self.parse_statement()
                if s: body.append(self._line_directive(tok2)+s)
            if self.current().type==TokenType.EOF:
                raise LuaBaseError("SyntaxError","Unterminated 'for'",t.line,t.col)
            self.expect(TokenType.END,False)
            self.var_types[var]="int"
            inner="\n    ".join(body) if body else ""
            return (f"for(int {var}=({start});"
                    f"({step})>=0?{var}<=({limit}):{var}>=({limit});"
                    f"{var}+=({step}))"
                    f"{{\n    {inner}\n}}")

        # F02: break
        if t.type==TokenType.BREAK: self.advance(); return "break;"

        # F03: continue
        if t.type==TokenType.CONTINUE: self.advance(); return "continue;"

        # F12: switch expr { case v: ... default: ... }
        if t.type==TokenType.SWITCH:
            self.advance(); expr=self.parse_expr(); self.expect(TokenType.LBRACE,False)
            cases=[]; default_stmts=[]
            while self.current().type not in (TokenType.RBRACE,TokenType.EOF):
                if self.current().type==TokenType.CASE:
                    self.advance(); val=self.parse_expr(); self.expect(TokenType.COLON,False)
                    stmts=[]
                    while self.current().type not in (TokenType.CASE,TokenType.DEFAULT_KW,TokenType.RBRACE,TokenType.EOF):
                        s=self.parse_statement()
                        if s: stmts.append("    "+s)
                    cases.append(f"case {val}:\n"+"\n".join(stmts)+"\n    break;")
                elif self.current().type==TokenType.DEFAULT_KW:
                    self.advance()
                    if self.current().type==TokenType.COLON: self.advance()
                    while self.current().type not in (TokenType.RBRACE,TokenType.EOF):
                        s=self.parse_statement()
                        if s: default_stmts.append("    "+s)
                else: self.advance()
            if self.current().type==TokenType.EOF:
                raise LuaBaseError("SyntaxError","Unterminated 'switch'",t.line,t.col)
            self.expect(TokenType.RBRACE,False)
            body="\n".join(cases)
            if default_stmts: body+="\ndefault:\n"+"\n".join(default_stmts)+"\n    break;"
            return f"switch({expr}){{\n{body}\n}}"

        # F23: strcpy / strcat as statements
        if t.type==TokenType.STRCPY_KW:
            self.advance(); self.expect(TokenType.LPAREN,False)
            dst=self.parse_expr(); self.expect(TokenType.COMMA,False)
            src=self.parse_expr(); self.expect(TokenType.RPAREN,False)
            return f"strcpy({dst},{src});"
        if t.type==TokenType.STRCAT_KW:
            self.advance(); self.expect(TokenType.LPAREN,False)
            dst=self.parse_expr(); self.expect(TokenType.COMMA,False)
            src=self.parse_expr(); self.expect(TokenType.RPAREN,False)
            return f"strcat({dst},{src});"

        # F24: memset
        if t.type==TokenType.MEMSET_KW:
            self.advance(); self.expect(TokenType.LPAREN,False)
            ptr=self.parse_expr(); self.expect(TokenType.COMMA,False)
            val=self.parse_expr(); self.expect(TokenType.COMMA,False)
            n=self.parse_expr();   self.expect(TokenType.RPAREN,False)
            return f"memset({ptr},{val},{n});"

        # Pointer dereference assignment: *name = expr
        if t.type==TokenType.MULTIPLY:
            self.advance(); name=self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)
            self.expect(TokenType.ASSIGN,False); val=self.parse_expr()
            return f"*{name} = {val};"

        # Identifier: assignment, compound assignment, ++/--, call
        if t.type==TokenType.IDENTIFIER:
            name=self.safe_name(self.advance().value)
            while self.current().type in (TokenType.LSBRACKET,TokenType.DOT,TokenType.ARROW):
                if self.current().type==TokenType.LSBRACKET:
                    self.advance(); idx=self.parse_expr(); self.expect(TokenType.RSBRACKET,False)
                    name=f"{name}[(int)({idx})]"
                elif self.current().type==TokenType.DOT:
                    self.advance(); field=self.expect(TokenType.IDENTIFIER,False).value
                    base=name.replace('[',' ').replace('.',' ').replace('->',' ').split()[0]
                    op="->" if self.var_types.get(base)=="ptr" else "."
                    name=f"{name}{op}{field}"
                elif self.current().type==TokenType.ARROW:
                    self.advance(); field=self.expect(TokenType.IDENTIFIER,False).value
                    name=f"{name}->{field}"
            if self.current().type==TokenType.ASSIGN:
                self.advance(); return f"{name} = {self.parse_expr()};"
            # F18: compound assignment
            compound={
                TokenType.PLUS_ASSIGN:"+=",TokenType.MINUS_ASSIGN:"-=",
                TokenType.MUL_ASSIGN:"*=",TokenType.DIV_ASSIGN:"/=",TokenType.MOD_ASSIGN:"%=",
            }
            if self.current().type in compound:
                cop=compound[self.advance().type]; return f"{name} {cop} {self.parse_expr()};"
            # F19: postfix ++/--
            if self.current().type==TokenType.INCR: self.advance(); return f"{name}++;"
            if self.current().type==TokenType.DECR: self.advance(); return f"{name}--;"
            # function call as statement
            if self.current().type==TokenType.LPAREN:
                self.advance(); args=[]
                while self.current().type!=TokenType.RPAREN:
                    args.append(self.parse_expr())
                    if self.current().type==TokenType.COMMA: self.advance()
                self.expect(TokenType.RPAREN,False)
                return f"{name}({', '.join(args)});"
            return ""

        # scanf
        if t.type==TokenType.SCANF:
            self.advance(); self.expect(TokenType.LPAREN,False)
            name=self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)
            self.expect(TokenType.RPAREN,False)
            return self._emit_scanf(name, self.var_types.get(name,""), t)

        # return
        if t.type==TokenType.RETURN:
            self.advance()
            if self.current().type in (TokenType.END,TokenType.RBRACE,TokenType.EOF,TokenType.SEMICOLON):
                return "return;"
            return f"return {self.parse_expr()};"

        # F19: prefix ++/-- as statement
        if t.type==TokenType.INCR:
            self.advance(); name=self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)
            return f"++{name};"
        if t.type==TokenType.DECR:
            self.advance(); name=self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)
            return f"--{name};"

        self.advance(); return ""

    # BUG-E / BUG-F / BUG-G / BUG-M FIXED
    def _emit_scanf(self, name, vt, tok):
        if vt in ("str","char*"):
            return (f'fgets({name},sizeof({name}),stdin);\n'
                    f'    {name}[strcspn({name},"\\n")]="\\0";')
        elif vt=="int":    return f'scanf("%d",&{name});'
        elif vt=="short":  return f'scanf("%hd",&{name});'   # BUG-E FIXED
        elif vt=="long":   return f'scanf("%ld",&{name});'   # BUG-E FIXED
        elif vt=="float":  return f'scanf("%f",&{name});'
        elif vt=="double": return f'scanf("%lf",&{name});'   # BUG-E/BUG-F FIXED: %lf required
        elif vt=="char":   return f'scanf(" %c",&{name});'   # F29
        elif vt=="bool":   return f'{{int _lb_t;scanf("%d",&_lb_t);{name}=(bool)_lb_t;}}'
        elif vt in ("__m256","__m256i"):
            # BUG-G FIXED: hard error instead of silent garbage write
            return (f'fprintf(stderr,"[LuaBase] line {tok.line}: '
                    f'cannot scanf into __m256/__m256i \'{name}\'\\n");exit(1);')
        else:
            return f'scanf("%d",&{name});/*LuaBase:unknown type for {name},defaulted %d*/'

    # -----------------------------------------------------------------------
    # Expression parser
    # Precedence (low->high): ternary -> logical -> bitor -> bitxor -> bitand
    #   -> comparison -> shift -> additive -> multiplicative -> unary -> power -> primary
    # -----------------------------------------------------------------------
    def parse_expr(self):         return self.parse_ternary()

    # F17: ternary
    def parse_ternary(self):
        c = self.parse_logical()
        if self.current().type==TokenType.QUESTION:
            self.advance(); t=self.parse_logical()
            self.expect(TokenType.COLON,False); e=self.parse_logical()
            return f"({c}?{t}:{e})"
        return c

    def parse_logical(self):
        left=self.parse_bitwise_or()
        while self.current().type in (TokenType.AND,TokenType.OR):
            op="&&" if self.advance().type==TokenType.AND else "||"
            right=self.parse_bitwise_or(); left=f"({left} {op} {right})"
        return left

    # F07: |
    def parse_bitwise_or(self):
        left=self.parse_bitwise_xor()
        while self.current().type==TokenType.BITOR:
            self.advance(); right=self.parse_bitwise_xor(); left=f"({left}|{right})"
        return left

    # F08: ^
    def parse_bitwise_xor(self):
        left=self.parse_bitwise_and()
        while self.current().type==TokenType.BITXOR:
            self.advance(); right=self.parse_bitwise_and(); left=f"({left}^{right})"
        return left

    # F06: & (binary; prefix & is ADDRESS_OF in parse_primary)
    def parse_bitwise_and(self):
        left=self.parse_comparison()
        while self.current().type==TokenType.ADDRESS_OF:
            self.advance(); right=self.parse_comparison(); left=f"({left}&{right})"
        return left

    def parse_comparison(self):
        left=self.parse_shift()
        cmp={TokenType.EQ:"==",TokenType.NE:"!=",TokenType.LT:"<",
             TokenType.GT:">",TokenType.LE:"<=",TokenType.GE:">="}
        while self.current().type in cmp:
            op=cmp[self.advance().type]; right=self.parse_shift(); left=f"({left}{op}{right})"
        return left

    # F09: << >>
    def parse_shift(self):
        left=self.parse_additive()
        while self.current().type in (TokenType.SHL,TokenType.SHR):
            op="<<" if self.advance().type==TokenType.SHL else ">>"
            right=self.parse_additive(); left=f"({left}{op}{right})"
        return left

    def parse_additive(self):
        left=self.parse_multiplicative()
        while self.current().type in (TokenType.PLUS,TokenType.MINUS):
            op=self.advance().value; right=self.parse_multiplicative(); left=f"({left}{op}{right})"
        return left

    def parse_multiplicative(self):
        left=self.parse_unary()
        while self.current().type in (TokenType.MULTIPLY,TokenType.DIVIDE,TokenType.MOD):
            op=self.advance().value; right=self.parse_unary(); left=f"({left}{op}{right})"
        return left

    def parse_unary(self):
        if self.current().type==TokenType.NOT:    self.advance(); return f"(!{self.parse_unary()})"
        if self.current().type==TokenType.BITNOT: self.advance(); return f"(~{self.parse_unary()})"
        if self.current().type==TokenType.MINUS:  self.advance(); return f"(-{self.parse_unary()})"
        if self.current().type==TokenType.MULTIPLY:
            self.advance(); return f"*{self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)}"
        if self.current().type==TokenType.INCR:
            self.advance(); return f"(++{self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)})"
        if self.current().type==TokenType.DECR:
            self.advance(); return f"(--{self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)})"
        return self.parse_power()

    # F20: ** -> pow()
    def parse_power(self):
        base=self.parse_primary()
        if self.current().type==TokenType.POW:
            self.advance(); exp=self.parse_unary()
            return f"pow((double)({base}),(double)({exp}))"
        return base

    def parse_primary(self):
        t=self.current()

        # F15: typeof(var) => string of type name
        if t.type==TokenType.TYPEOF:
            self.advance(); self.expect(TokenType.LPAREN,False)
            name=self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)
            self.expect(TokenType.RPAREN,False)
            return f'"{self.var_types.get(name,"unknown")}"'

        # F16: sizeof
        if t.type==TokenType.SIZEOF_KW:
            self.advance(); self.expect(TokenType.LPAREN,False)
            inner_tok=self.current()
            if inner_tok.type in (TokenType.INT,TokenType.FLOAT,TokenType.STR,
                                   TokenType.LONG,TokenType.SHORT,TokenType.DOUBLE,
                                   TokenType.BOOL_KW,TokenType.CHAR_KW,TokenType.IDENTIFIER):
                n=self.advance().value
                if n=="str": n="char*"
                self.expect(TokenType.RPAREN,False); return f"sizeof({n})"
            inner=self.parse_expr(); self.expect(TokenType.RPAREN,False)
            return f"sizeof({inner})"

        # F14: cast(type, expr)
        if t.type==TokenType.CAST:
            self.advance(); self.expect(TokenType.LPAREN,False)
            tn=self.advance().value
            if tn=="str": tn="char*"
            self.expect(TokenType.COMMA,False); inner=self.parse_expr()
            self.expect(TokenType.RPAREN,False)
            return f"(({tn})({inner}))"

        # F22: strlen(str)
        if t.type==TokenType.STRLEN_KW:
            self.advance(); self.expect(TokenType.LPAREN,False)
            arg=self.parse_expr(); self.expect(TokenType.RPAREN,False)
            return f"(int)strlen({arg})"

        # F25: exit in expression context
        if t.type==TokenType.EXIT_KW:
            self.advance(); self.expect(TokenType.LPAREN,False)
            code=self.parse_expr(); self.expect(TokenType.RPAREN,False)
            return f"(exit({code}),0)"

        if t.type==TokenType.LPAREN:
            self.advance(); inner=self.parse_expr(); self.expect(TokenType.RPAREN,False)
            return f"({inner})"

        if t.type==TokenType.ADDRESS_OF:
            self.advance(); return f"&{self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)}"

        t=self.advance()
        if t.type==TokenType.NUMBER:   return str(t.value)
        if t.type==TokenType.STRING:   return f'"{t.value}"'
        if t.type==TokenType.TRUE_KW:  return "true"
        if t.type==TokenType.FALSE_KW: return "false"
        if t.type==TokenType.CHAR_LIT: return f"'{t.value}'"

        if t.type==TokenType.IDENTIFIER:
            name=self.safe_name(t.value)
            if self.current().type==TokenType.LPAREN:
                self.advance(); args=[]
                while self.current().type!=TokenType.RPAREN:
                    if self.current().type==TokenType.EOF:
                        raise LuaBaseError("SyntaxError",
                            f"Unterminated args in call to '{name}'",t.line,t.col)
                    args.append(self.parse_expr())
                    if self.current().type==TokenType.COMMA: self.advance()
                self.expect(TokenType.RPAREN,False)
                return f"{name}({', '.join(args)})"
            while self.current().type in (TokenType.LSBRACKET,TokenType.DOT,TokenType.ARROW):
                if self.current().type==TokenType.LSBRACKET:
                    self.advance(); idx=self.parse_expr(); self.expect(TokenType.RSBRACKET,False)
                    name=f"{name}[(int)({idx})]"
                elif self.current().type==TokenType.DOT:
                    self.advance(); field=self.expect(TokenType.IDENTIFIER,False).value
                    base=name.replace('[',' ').replace('.',' ').replace('->',' ').split()[0]
                    op="->" if self.var_types.get(base)=="ptr" else "."
                    name=f"{name}{op}{field}"
                elif self.current().type==TokenType.ARROW:
                    self.advance(); field=self.expect(TokenType.IDENTIFIER,False).value
                    name=f"{name}->{field}"
            if self.current().type==TokenType.INCR: self.advance(); return f"({name}++)"
            if self.current().type==TokenType.DECR: self.advance(); return f"({name}--)"
            return name

        raise LuaBaseError("SyntaxError",
            f"Unexpected token {repr(t.value)} in expression",t.line,t.col)

    def parse_function(self):
        fn_tok=self.current(); self.advance()
        valid_ret={
            TokenType.INT,TokenType.FLOAT,TokenType.STR,TokenType.LONG,
            TokenType.SHORT,TokenType.DOUBLE,TokenType.VOID,
            TokenType.M256,TokenType.M256I,TokenType.IDENTIFIER,
            TokenType.BOOL_KW,TokenType.CHAR_KW,TokenType.PTR,
        }
        rt=self.current()
        if rt.type not in valid_ret:
            raise LuaBaseError("SyntaxError",
                f"Expected return type after 'function', got {repr(rt.value)}",rt.line,rt.col)
        raw_ret=self.advance().value
        if raw_ret=="ptr":
            inner_ret=self.advance().value
            ret_type=f"{inner_ret.replace('str','char*')}*"
        elif raw_ret=="str":   ret_type="char*"
        elif raw_ret=="bool":  ret_type="bool"
        elif raw_ret=="char":  ret_type="char"
        else:                  ret_type=raw_ret
        name=self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)
        self.expect(TokenType.LPAREN,False); params=[]
        while self.current().type!=TokenType.RPAREN:
            if self.current().type==TokenType.EOF:
                raise LuaBaseError("SyntaxError",
                    f"Unterminated params in function '{name}'",fn_tok.line,fn_tok.col)
            pt=self.current()
            valid_p={TokenType.INT,TokenType.FLOAT,TokenType.STR,TokenType.LONG,
                     TokenType.SHORT,TokenType.DOUBLE,TokenType.VOID,TokenType.PTR,
                     TokenType.M256,TokenType.M256I,TokenType.IDENTIFIER,
                     TokenType.BOOL_KW,TokenType.CHAR_KW}
            if pt.type not in valid_p:
                raise LuaBaseError("SyntaxError",
                    f"Expected param type in '{name}', got {repr(pt.value)}",pt.line,pt.col)
            p_type_raw=self.advance().value
            if p_type_raw=="ptr":
                inner=self.advance().value; p_type=f"{inner.replace('str','char*')}*"
            elif p_type_raw=="str":  p_type="char*"
            elif p_type_raw=="bool": p_type="bool"
            elif p_type_raw=="char": p_type="char"
            else:                    p_type=p_type_raw
            p_name=self.safe_name(self.expect(TokenType.IDENTIFIER,False).value)
            self.var_types[p_name]=p_type_raw; params.append(f"{p_type} {p_name}")
            if self.current().type==TokenType.COMMA: self.advance()
        self.expect(TokenType.RPAREN,False); self.expect(TokenType.LBRACE,False)
        body=[]
        while self.current().type not in (TokenType.RBRACE,TokenType.EOF):
            tok2=self.current(); s=self.parse_statement()
            if s: body.append(self._line_directive(tok2)+"    "+s)
        if self.current().type==TokenType.EOF:
            raise LuaBaseError("SyntaxError",
                f"Unterminated function '{name}'",fn_tok.line,fn_tok.col)
        self.expect(TokenType.RBRACE,False)
        return f"{ret_type} {name}({', '.join(params)}) {{\n"+"\n".join(body)+"\n}\n"


# ===========================================================================
# CLI entry point
# ===========================================================================
if __name__ == "__main__":
    priCMD = False
    debug=False; clang_cmd=[]
    import os,sys,subprocess,tempfile,random,string,shutil
    shut="--shut" in sys.argv or "-s" in sys.argv

    def log(what):
        if not shut: print(str(what).strip())
    def die(msg,code=-1):
        print(f"[-] {msg}",file=sys.stderr); sys.exit(code)

    getasm="--asm" in sys.argv
    if len(sys.argv)<3:
        die("Usage: luabasec <in.lb> <out> [extra.c] [-lPATH] [-gLIB] [-wLIBDIR] [-c] [-s] [--asm]",1)

    inf,out_bin=sys.argv[1],sys.argv[2]
    if not os.path.exists(inf): die(f"Input file not found: '{inf}'",1)
    if not inf.endswith(".lb"): log(f"[!] Warning: '{inf}' does not have a .lb extension")
    base=os.path.splitext(inf)[0]
    custom_includes=[]; extra_c_files=[]

    for arg in sys.argv[3:]:
        if arg.startswith("-l"):
            path=arg[2:].strip('"').strip("'"); folder=os.path.dirname(path)
            if folder: custom_includes.append(f"-I{folder}")
        if arg.startswith("-g"):
            path=arg[2:].strip('"').strip("'"); custom_includes.append(f"-l{path}")
        if arg.endswith(".c") or arg.endswith(".o"):
            if os.path.exists(arg): extra_c_files.append(arg)
            else: log(f"[-] Warning: Extra source file '{arg}' not found.")
        if arg=="-c": debug=True
        if arg.startswith("-w"):
            path=arg[2:].strip('"').strip("'"); custom_includes.append(f"-L{path}")
        if arg=="-asm": getasm=True
        if arg=='-sCMD': priCMD = True

    with open(inf,'r',encoding='utf-8') as f: source=f.read()
    log("[*] Tokenizing...")
    try:
        tokens=Lexer(source).tokenize()
    except LuaBaseError as e: die(str(e))
    except Exception as e:    die(f"Unexpected error during tokenization: {e}")

    source_dir  = os.path.dirname(os.path.abspath(inf)) or "."
    source_file = os.path.abspath(inf)

    log("[*] Compiling...")
    try:
        c_code=CTranspiler(tokens).transpile(source_dir=source_dir,source_file=source_file)
    except LuaBaseError as e: die(str(e))
    except Exception as e:    die(f"Unexpected error during transpilation: {e}")

    c_file=(os.path.join(tempfile.gettempdir(),
            f"{base}-{''.join(random.choices(string.ascii_letters,k=6))}.c")
            if not debug else f"{inf}.c")
    try:
        with open(c_file,'w',encoding='utf-8') as f: f.write(c_code)
    except OSError as e: die(f"Could not write C file '{c_file}': {e}")

    clang_bin=shutil.which("clang")
    if clang_bin is None:
        print("[-] clang is not installed.",file=sys.stderr)
        try: answer=input("    Install clang now? [y/N] ").strip().lower()
        except (EOFError,KeyboardInterrupt): answer=""
        if answer=="y":
            import platform; system=platform.system()
            if system=="Linux":
                if shutil.which("apt"):     subprocess.run(["sudo","apt","install","-y","clang","lld"],check=True)
                elif shutil.which("dnf"):   subprocess.run(["sudo","dnf","install","-y","clang","lld"],check=True)
                elif shutil.which("pacman"):subprocess.run(["sudo","pacman","-S","--noconfirm","clang","lld"],check=True)
                else: die("No supported package manager. Install clang manually.")
            elif system=="Darwin": subprocess.run(["brew","install","llvm"],check=True)
            elif system=="Windows": subprocess.run(["winget","install","LLVM.LLVM"],check=True)
            else: die(f"Unsupported OS '{system}'.")
            clang_bin=shutil.which("clang")
            if clang_bin is None: die("clang still not found on PATH after install.")
        else: die("clang is required.")

    clang_cmd=[clang_bin,c_file]+extra_c_files+["-o",out_bin,"-lm"]+custom_includes
    clang_cmd+=["-ffast-math","-march=native","-w","-O3","-fuse-ld=lld","-mavx2"]
    if not getasm: clang_cmd+=["-flto"]
    if getasm:     clang_cmd.append("-S")
    clang_cmd+=[f"-I{source_dir}"]

    log("[*] Compiling C code...")
    if debug: log("Written to debug file.")
    elif priCMD:     log(f"[*] Running: {' '.join(clang_cmd)}")
    else:            log(f"[*] Running clang to produce ./{out_bin}...")

    local_debug_file=f"{base}_debug.c"
    try:
        if not debug:
            result=subprocess.run(clang_cmd,capture_output=True,text=True)
            if result.returncode==0:
                log(f"[*] Made ./{out_bin}")
            else:
                shutil.copy(c_file,local_debug_file)
                print(f"[-] Clang error (exit {result.returncode}). Debug C -> ./{local_debug_file}",file=sys.stderr)
                if result.stderr.strip(): print(result.stderr.strip(),file=sys.stderr)
                if result.stdout.strip(): print(result.stdout.strip(),file=sys.stderr)
                sys.exit(-2)
    except FileNotFoundError: die(f"Could not execute clang at '{clang_bin}'")
    except OSError as e:      die(f"OS error while running clang: {e}")
    finally:
        if os.path.exists(c_file) and not debug:
            try: os.remove(c_file)
            except OSError: pass

)LB";

// ===========================================================================
// C wrapper
// ===========================================================================
int main(int argc, char** argv)
{
    Py_Initialize();

    // EXTRA: NULL-check after PyMem_RawMalloc (OOM guard)
    wchar_t** w_argv = (wchar_t**)PyMem_RawMalloc(sizeof(wchar_t*) * (size_t)argc);
    if (!w_argv) {
        fprintf(stderr, "[-] Out of memory\n");
        Py_Finalize();
        return 1;
    }
    for (int i = 0; i < argc; i++)
        w_argv[i] = Py_DecodeLocale(argv[i], NULL);
    PySys_SetArgvEx(argc, w_argv, 0);

    int py_ret    = PyRun_SimpleString(transpiler_logic);
    int exit_code = 0;

    if (py_ret != 0)
    {
        PyObject* exc = PyErr_Occurred();
        if (exc && PyErr_GivenExceptionMatches(exc, PyExc_SystemExit))
        {
            PyObject *ptype, *pvalue, *ptraceback;
            PyErr_Fetch(&ptype, &pvalue, &ptraceback);

            // BUG-I FIXED: pvalue can be NULL, None, PyLong, or PyUnicode.
            // PyLong_AsLong(None) sets a new exception and returns -1, which
            // then causes a CPython assertion failure inside Py_Finalize().
            if (pvalue == NULL || pvalue == Py_None)
                exit_code = 0;
            else if (PyLong_Check(pvalue))
                exit_code = (int)PyLong_AsLong(pvalue);
            else
                exit_code = 1;  // sys.exit("message") -> failure

            Py_XDECREF(ptype);
            Py_XDECREF(pvalue);
            Py_XDECREF(ptraceback);
        }
        else
        {
            PyErr_Print();
            exit_code = 1;
        }
    }

    // Free BEFORE Py_Finalize to avoid use-after-free
    for (int i = 0; i < argc; i++)
        PyMem_RawFree(w_argv[i]);
    PyMem_RawFree(w_argv);

    Py_Finalize();
    return exit_code;
}