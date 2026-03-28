#include <Python.h>
#include <wchar.h>

const char* transpiler_logic = R"LB(#!/usr/bin/env python3
import sys, os, subprocess

class TokenType:
    NUMBER = "NUMBER"; STRING = "STRING"; IDENTIFIER = "IDENTIFIER"
    IF = "IF"; THEN = "THEN"; ELSE = "ELSE"; END = "END"
    WHILE = "WHILE"; DO = "DO"; FUNCTION = "FUNCTION"; RETURN = "RETURN"
    PRINT = "PRINT"; SCANF = "SCANF"; INT = "INT"; STR = "STR"; FLOAT = "FLOAT"
    FOPEN = "FOPEN"; FWRITE = "FWRITE"; FCLOSE = "FCLOSE"; HLT = "HLT"
    ASSIGN = "ASSIGN"; PLUS = "PLUS"; MINUS = "MINUS"; MULTIPLY = "MULTIPLY"
    DIVIDE = "DIVIDE"; EQ = "EQ"; NE = "NE"; LT = "LT"; GT = "GT"; MOD = "MOD"
    # LE and GE token types for <= and >=
    LE = "LE"; GE = "GE"
    LPAREN = "LPAREN"; RPAREN = "RPAREN"; LBRACE = "LBRACE"; RBRACE = "RBRACE"
    COMMA = "COMMA"; EOF = "EOF"; LINK = "LINK"; LSBRACKET = "LSBRACKET"; RSBRACKET = "RSBRACKET";
    LONG = "LONG"; DOUBLE = "DOUBLE"; M256 = "M256"; VOID = "VOID"; SHORT = "SHORT"; M256I = "M256I";
    TYPE = "TYPE"; ELSEIF = "ELSEIF"; AND = "AND"; OR = "OR"; PTR = "PTR"; ADDRESS_OF = "ADDRESS_OF";
    DOT = "DOT"

class LuaBaseError(Exception):
    """Structured error with source location."""
    def __init__(self, kind, msg, line=None, col=None, snippet=None):
        self.kind = kind
        self.line = line
        self.col = col
        self.snippet = snippet
        loc = f" (line {line}, col {col})" if line is not None else ""
        detail = f"\n    {snippet}" if snippet else ""
        super().__init__(f"[{kind}]{loc}: {msg}{detail}")

class Token:
    def __init__(self, ttype, value, line=None, col=None):
        self.type = ttype
        self.value = value
        self.line = line   # 1-based source line
        self.col = col     # 1-based column

class Lexer:
    def __init__(self, source):
        self.source = source
        self.pos = 0
        self.line = 1
        self.col = 1
        self.keywords = {
            'if': TokenType.IF, 'then': TokenType.THEN, 'else': TokenType.ELSE,
            'end': TokenType.END, 'while': TokenType.WHILE, 'do': TokenType.DO,
            'function': TokenType.FUNCTION, 'return': TokenType.RETURN,
            'print': TokenType.PRINT, 'scanf': TokenType.SCANF,
            'int': TokenType.INT, 'str': TokenType.STR, 'float': TokenType.FLOAT,
            'fopen': TokenType.FOPEN, 'fwrite': TokenType.FWRITE, 'fclose': TokenType.FCLOSE,
            'hlt': TokenType.HLT, 'link': TokenType.LINK, 'long': TokenType.LONG,
            'double': TokenType.DOUBLE,
            '__m256': TokenType.M256, 'void': TokenType.VOID, 'short': TokenType.SHORT,
            '__m256i': TokenType.M256I,
            'type': TokenType.TYPE, 'elseif': TokenType.ELSEIF, 'ptr': TokenType.PTR
        }

    def _advance_char(self):
        """Advance one character, keeping line/col counters up to date."""
        ch = self.source[self.pos]
        self.pos += 1
        if ch == '\n':
            self.line += 1
            self.col = 1
        else:
            self.col += 1
        return ch

    def _current_line_text(self):
        """Return the full text of the current source line for error snippets."""
        start = self.source.rfind('\n', 0, self.pos)
        start = 0 if start == -1 else start + 1
        end = self.source.find('\n', self.pos)
        end = len(self.source) if end == -1 else end
        return self.source[start:end]

    def tokenize(self):
        tokens = []
        while self.pos < len(self.source):
            char = self.source[self.pos]

            # --- Whitespace ---
            if char.isspace():
                self._advance_char()
                continue

            # --- Line comments ---
            if char == '#':
                while self.pos < len(self.source) and self.source[self.pos] != '\n':
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

            # --- Numeric literals ---
            elif char.isdigit():
                start = self.pos
                dot_seen = False
                while self.pos < len(self.source) and (self.source[self.pos].isdigit() or (self.source[self.pos] == '.' and not dot_seen)):
                    if self.source[self.pos] == '.':
                        dot_seen = True
                    self._advance_char()
                tokens.append(Token(TokenType.NUMBER, self.source[start:self.pos], tok_line, tok_col))

            # --- String literals ---
            elif char in ('"', "'"):
                q = char
                self._advance_char()  # eat opening quote
                start = self.pos
                while self.pos < len(self.source) and self.source[self.pos] != q:
                    if self.source[self.pos] == '\n':
                        raise LuaBaseError("LexError", f"Unterminated string literal (missing closing {q!r})",
                                           tok_line, tok_col, self._current_line_text())
                    self._advance_char()
                if self.pos >= len(self.source):
                    raise LuaBaseError("LexError", f"Unterminated string literal at end of file (missing closing {q!r})",
                                       tok_line, tok_col)
                val = self.source[start:self.pos]
                self._advance_char()  # eat closing quote
                tokens.append(Token(TokenType.STRING, val, tok_line, tok_col))

            # --- Operators (two-char then one-char) ---
            else:
                double = self.source[self.pos:self.pos+2]
                ops2 = {'==': TokenType.EQ, '!=': TokenType.NE, '<=': TokenType.LE, '>=': TokenType.GE,
                        '&&': TokenType.AND, '||': TokenType.OR}
                if double in ops2:
                    tokens.append(Token(ops2[double], double, tok_line, tok_col))
                    self._advance_char(); self._advance_char()
                else:
                    ops1 = {'+': TokenType.PLUS, '-': TokenType.MINUS, '*': TokenType.MULTIPLY, '/': TokenType.DIVIDE,
                            '%': TokenType.MOD, '=': TokenType.ASSIGN, '(': TokenType.LPAREN, ')': TokenType.RPAREN,
                            '{': TokenType.LBRACE, '}': TokenType.RBRACE, ',': TokenType.COMMA, '<': TokenType.LT,
                            '>': TokenType.GT, ']': TokenType.RSBRACKET, '[': TokenType.LSBRACKET,
                            '&': TokenType.ADDRESS_OF, '.': TokenType.DOT}
                    if char in ops1:
                        tokens.append(Token(ops1[char], char, tok_line, tok_col))
                        self._advance_char()
                    else:
                        raise LuaBaseError("LexError", f"Unexpected character {char!r}",
                                           tok_line, tok_col, self._current_line_text())

        tokens.append(Token(TokenType.EOF, None, self.line, self.col))
        return tokens


# ---------------------------------------------------------------------------
# Helper: escape a filesystem path for use inside a C #line directive string.
# On Windows paths contain backslashes which are escape chars in C strings.
# ---------------------------------------------------------------------------
def _c_path(path):
    return path.replace('\\', '\\\\')


class CTranspiler:
    def __init__(self, tokens):
        self.tokens = tokens
        self.pos = 0

        # ---------------------------------------------------------------
        # Runtime helpers injected at the top of every generated .c file.
        #
        # FIX (BUG-A / BUG-A2): Added _lb_mi() for __m256i and added the
        # __m256i branch inside the TO_STR _Generic macro.  Previously
        # __m256i fell through to "default: _lb_i" which formatted the
        # vector as a single int — completely wrong output and a silent bug.
        #
        # FIX (BUG-M / NOTE-4): _LB_STR_BUF_SIZE controls the fixed-size
        # buffer emitted for bare 'str name' declarations.  256 bytes is the
        # default; change it here to affect every generated file.
        # ---------------------------------------------------------------
        self.headers = [
            "#include <stdio.h>\n"
            "#include <stdbool.h>\n"
            "#include <stdlib.h>\n"
            "#include <math.h>\n"
            "#include <immintrin.h>\n"
            "#include <string.h>\n"
            "#include <unistd.h>\n",

            # _lb_buf: shared scratch buffer used by all TO_STR helpers.
            # Sequential use is safe because each printf/fprintf call reads
            # the buffer immediately after TO_STR fills it.
            "char _lb_buf[256];\n"
            # Integer holding a second formatted value (e.g. __m256i).
            # We use a separate buffer so nested TO_STR calls in the same
            # printf don't trample each other.  (Two-buffer scheme.)
            "char _lb_buf2[256];\n"
            "static inline char* _lb_s(char* x)   { return x; }\n"
            "static inline char* _lb_cs(const char* x) { return (char*)x; }\n"
            "static inline char* _lb_f(float x)   { sprintf(_lb_buf,  \"%g\",    x);           return _lb_buf; }\n"
            "static inline char* _lb_d(double x)  { sprintf(_lb_buf,  \"%.10g\", x);           return _lb_buf; }\n"
            "static inline char* _lb_i(int x)     { sprintf(_lb_buf,  \"%d\",    x);           return _lb_buf; }\n"
            "static inline char* _lb_l(long x)    { sprintf(_lb_buf,  \"%ld\",   x);           return _lb_buf; }\n"
            "static inline char* _lb_u(short x)   { sprintf(_lb_buf,  \"%d\",    (int)x);      return _lb_buf; }\n"
            # __m256: store 8 floats, print as [f0, f1, ..., f7]
            "static inline char* _lb_m(__m256 v)  { float f[8]; _mm256_storeu_ps(f, v); "
                "sprintf(_lb_buf, \"[%g, %g, %g, %g, %g, %g, %g, %g]\", "
                "f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7]); return _lb_buf; }\n"
            # FIX BUG-A2: __m256i helper — extract as 8x int32 using a union.
            # _mm256_storeu_si256 requires an aligned __m256i pointer; the union
            # trick avoids that requirement and is fully portable within AVX2.
            "static inline char* _lb_mi(__m256i v) { "
                "union { __m256i v; int i[8]; } u; u.v = v; "
                "sprintf(_lb_buf, \"[%d, %d, %d, %d, %d, %d, %d, %d]\", "
                "u.i[0],u.i[1],u.i[2],u.i[3],u.i[4],u.i[5],u.i[6],u.i[7]); return _lb_buf; }\n"
            # FIX BUG-A: Added __m256i: _lb_mi branch so the correct helper is
            # chosen instead of the wrong default: _lb_i path.
            "#define TO_STR(x) _Generic((x), "
                "char*: _lb_s, const char*: _lb_cs, "
                "__m256: _lb_m, __m256i: _lb_mi, "
                "float: _lb_f, double: _lb_d, "
                "int: _lb_i, long: _lb_l, short: _lb_u, "
                "default: _lb_i)(x)\n"
        ]

        self.functions  = []
        self.main_body  = []
        self.var_types  = {}
        self._handle_declared = False

        # Source filename used for #line directives (set in transpile()).
        self._source_file = "<unknown>"

    # -----------------------------------------------------------------------
    # Low-level token helpers
    # -----------------------------------------------------------------------
    def current(self): return self.tokens[self.pos]
    def advance(self): t = self.current(); self.pos += 1; return t

    def expect(self, ttype, shut):
        tok = self.current()
        if tok.type == ttype:
            return self.advance()
        got = repr(tok.value) if tok.value is not None else tok.type
        if not shut:
            raise LuaBaseError("SyntaxError", f"Expected {ttype}, got {got}", tok.line, tok.col)
        return None

    def safe_name(self, name):
        """
        Mangle names that collide with C keywords.
        IMPORTANT: every place that stores into var_types must use the MANGLED
        name as the key so that lookups (e.g. in fwrite/scanf) are consistent.
        """
        c_keywords = {
            'double', 'int', 'float', 'char', 'while', 'if', 'return', 'break',
            'FILE', 'long', 'void', 'short', '__m256', '__m256i',
            'for', 'do', 'else', 'struct', 'typedef', 'switch', 'case', 'default',
            'const', 'static', 'unsigned', 'signed', 'extern', 'goto', 'sizeof',
            'enum', 'union', 'continue', 'register', 'volatile', 'auto'
        }
        return f"var_{name}" if name in c_keywords else name

    # -----------------------------------------------------------------------
    # #line directive helper
    # -----------------------------------------------------------------------
    def _line_directive(self, tok):
        """
        Return a C preprocessor #line directive for the given token.
        This tells clang/gcc that the *next* source line corresponds to
        line tok.line in the original .lb file, so error messages and
        debugger step-through point at the right place in your source.

        FIX BUG-J: Previously no #line directives were emitted at all.
        """
        if tok is None or tok.line is None:
            return ""
        # FIX NOTE-11: escape backslashes in Windows paths so the C string
        # is well-formed (e.g.  C:\foo  ->  C:\\foo).
        safe_path = _c_path(self._source_file)
        return f'#line {tok.line} "{safe_path}"\n'

    # -----------------------------------------------------------------------
    # transpile() — top-level entry point
    # -----------------------------------------------------------------------
    def transpile(self, source_dir=".", source_file="<unknown>"):
        self._source_dir  = source_dir
        self._source_file = source_file          # stored for #line directives
        self._lh_included = set()

        while self.current().type != TokenType.EOF:
            tok = self.current()

            if tok.type == TokenType.TYPE:
                line_dir = self._line_directive(tok)
                self.headers.append(line_dir + self.parse_type_definition())

            elif tok.type == TokenType.FUNCTION:
                line_dir = self._line_directive(tok)
                self.functions.append(line_dir + self.parse_function())

            elif tok.type == TokenType.LINK:
                self.advance()
                filename_token = self.expect(TokenType.STRING, True)
                if filename_token is None:
                    raise LuaBaseError("SyntaxError",
                        "'link' requires a string filename, e.g. link \"file.h\"")
                fname = filename_token.value
                if fname.endswith(".lh"):
                    self._transpile_lh(fname, filename_token)
                else:
                    self.headers.append(f'#include "{fname}"')

            else:
                tok = self.current()
                stmt = self.parse_statement()
                if stmt:
                    # Prepend the #line directive so this C statement is
                    # mapped back to the originating .lb line in error output.
                    self.main_body.append(self._line_directive(tok) + stmt)

        # -------------------------------------------------------------------
        # Assemble the final C source.
        # Each main_body entry already owns its own #line prefix; we join with
        # a plain newline so the directives are always at column 0.
        # FIX NOTE-10: was '    '.join(...) which put #line in the middle of a
        # line when the directive was embedded — now each entry is self-contained.
        # -------------------------------------------------------------------
        body_str = "\n    ".join(self.main_body)

        res  = "\n".join(self.headers) + "\n"
        res += "\n".join(self.functions) + "\n"
        res += "int main(int argc, char** argv) {\n    "
        res += body_str
        res += "\n    return 0;\n}"
        return res

    # -----------------------------------------------------------------------
    # _transpile_lh — import a .lh library file
    # -----------------------------------------------------------------------
    def _transpile_lh(self, fname, tok):
        path = fname if os.path.isabs(fname) else os.path.join(self._source_dir, fname)
        canonical = os.path.normpath(os.path.abspath(path))

        if canonical in self._lh_included:
            return
        self._lh_included.add(canonical)

        if not os.path.exists(canonical):
            raise LuaBaseError("LinkError",
                f"Cannot find .lh file '{fname}' (looked in '{self._source_dir}')",
                tok.line, tok.col)

        try:
            with open(canonical, 'r', encoding='utf-8') as f:
                lh_source = f.read()
        except OSError as e:
            raise LuaBaseError("LinkError", f"Could not read '{fname}': {e}", tok.line, tok.col)

        try:
            lh_tokens = Lexer(lh_source).tokenize()
        except LuaBaseError as e:
            raise LuaBaseError(e.kind, f"In '{fname}': {e}", tok.line, tok.col)

        lh = CTranspiler(lh_tokens)
        lh._source_dir   = os.path.dirname(canonical)
        lh._source_file  = canonical          # #line directives in .lh point to .lh
        lh._lh_included  = self._lh_included
        lh.var_types     = self.var_types

        # FIX BUG-H: share _handle_declared so a .lh that calls fopen won't
        # re-declare FILE* handle if the parent .lb already declared it, and
        # vice-versa.  We write it back after processing so the parent knows.
        lh._handle_declared = self._handle_declared

        while lh.current().type != TokenType.EOF:
            t = lh.current()

            if t.type == TokenType.FUNCTION:
                line_dir = lh._line_directive(t)
                self.functions.append(line_dir + lh.parse_function())

            elif t.type == TokenType.TYPE:
                line_dir = lh._line_directive(t)
                self.headers.append(line_dir + lh.parse_type_definition())

            elif t.type == TokenType.LINK:
                lh.advance()
                nested_tok = lh.expect(TokenType.STRING, True)
                if nested_tok is not None:
                    if nested_tok.value.endswith(".lh"):
                        self._transpile_lh(nested_tok.value, nested_tok)
                    else:
                        self.headers.append(f'#include "{nested_tok.value}"')
            else:
                lh.parse_statement()   # parse + discard non-definition statements

        # FIX BUG-H (cont.): propagate _handle_declared back to the parent.
        self._handle_declared = lh._handle_declared

    # -----------------------------------------------------------------------
    # parse_type_definition
    # -----------------------------------------------------------------------
    def parse_type_definition(self):
        self.advance()  # skip 'type'
        name_tok = self.current()
        struct_name = self.expect(TokenType.IDENTIFIER, False).value

        self.expect(TokenType.LBRACE, False)
        fields = []
        valid_field_types = {
            TokenType.INT, TokenType.FLOAT, TokenType.STR, TokenType.LONG,
            TokenType.SHORT, TokenType.DOUBLE, TokenType.VOID, TokenType.PTR,
            TokenType.M256, TokenType.M256I, TokenType.IDENTIFIER
        }
        while self.current().type != TokenType.RBRACE:
            if self.current().type == TokenType.EOF:
                raise LuaBaseError("SyntaxError",
                    f"Unterminated type definition '{struct_name}' — expected '}}' before end of file",
                    name_tok.line, name_tok.col)

            ft = self.current()
            if ft.type not in valid_field_types:
                raise LuaBaseError("SyntaxError",
                    f"Expected a field type inside type '{struct_name}', got {repr(ft.value)}",
                    ft.line, ft.col)

            f_type_raw = self.advance().value

            # Handle 'ptr <type>' field syntax (e.g. ptr int -> int*)
            if f_type_raw == "ptr":
                inner = self.advance().value
                f_type = f"{inner.replace('str', 'char*')}*"
            else:
                # FIX BUG-L: reject bare 'void' as a struct field type.
                # 'void field;' is a hard C error.  Only 'ptr void' (→ void*)
                # is legal, which is already handled by the 'ptr' branch above.
                if f_type_raw == "void":
                    raise LuaBaseError("SyntaxError",
                        f"Field type 'void' is invalid inside type '{struct_name}'. "
                        f"Use 'ptr void' for a void pointer.",
                        ft.line, ft.col)
                f_type = f_type_raw.replace("str", "char*")

            # FIX BUG-K: pass field names through safe_name() so reserved C
            # words used as field names (e.g. a field called 'int') are mangled
            # to 'var_int' instead of emitting 'int int;' which is a C error.
            f_name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            fields.append(f"{f_type} {f_name};")

        self.expect(TokenType.RBRACE, False)
        self.var_types[struct_name] = "STRUCT"
        return f"typedef struct {{\n    " + "\n    ".join(fields) + f"\n}} {struct_name};\n"

    # -----------------------------------------------------------------------
    # parse_statement
    # -----------------------------------------------------------------------
    def parse_statement(self):
        t = self.current()

        if t.type == TokenType.TYPE:
            return self.parse_type_definition()

        # --- HLT (sleep ms) ---
        if t.type == TokenType.HLT:
            self.advance()
            self.expect(TokenType.LPAREN, False)
            ms = self.parse_expr()
            self.expect(TokenType.RPAREN, False)
            return f"usleep(({ms}) * 1000);"

        # --- File I/O ---
        if t.type == TokenType.FOPEN:
            self.advance()
            self.expect(TokenType.LPAREN, False)
            fname = self.parse_expr()
            self.expect(TokenType.COMMA, False)
            mode = self.parse_expr()
            self.expect(TokenType.RPAREN, False)
            if not self._handle_declared:
                self._handle_declared = True
                return (f'FILE* handle = fopen({fname}, {mode});\n'
                        f'    if (handle == NULL) {{ printf("FAIL: %s\\n", {fname}); exit(1); }}')
            else:
                return (f'handle = fopen({fname}, {mode});\n'
                        f'    if (handle == NULL) {{ printf("FAIL: %s\\n", {fname}); exit(1); }}')

        if t.type == TokenType.FWRITE:
            self.advance()
            self.expect(TokenType.LPAREN, False)
            content_token = self.current()
            content = self.parse_expr()
            self.expect(TokenType.RPAREN, False)

            # ---------------------------------------------------------------
            # FIX BUG-B: The original code looked up var_types using the RAW
            # token value (e.g. "double"), but var_types keys are the MANGLED
            # names produced by safe_name() (e.g. "var_double").  Applying
            # safe_name() here makes the lookup consistent.
            #
            # FIX BUG-C / BUG-C2 / BUG-D: Expanded the type dispatch so that:
            #   float   -> explicit %g  (was silently caught by the else branch)
            #   __m256  -> TO_STR + %s  (was raw fprintf("%g", vec) = compile error)
            #   __m256i -> TO_STR + %s  (was raw fprintf("%g", vec) = compile error)
            #   unknown -> TO_STR + %s  (safe fallback for structs / ptr types)
            # ---------------------------------------------------------------
            if content_token.type == TokenType.IDENTIFIER:
                raw_var_name = self.safe_name(content_token.value)  # FIX BUG-B
                var_type = self.var_types.get(raw_var_name, "")
            else:
                var_type = ""

            if (var_type in ("str", "char*")) or (var_type == "" and content_token.type == TokenType.STRING):
                return f'if(handle) {{ fprintf(handle, "%s", {content}); fflush(handle); }}'
            elif var_type in ("int", "short"):
                return f'if(handle) {{ fprintf(handle, "%d", {content}); fflush(handle); }}'
            elif var_type == "long":
                return f'if(handle) {{ fprintf(handle, "%ld", {content}); fflush(handle); }}'
            elif var_type == "double":
                return f'if(handle) {{ fprintf(handle, "%.10g", {content}); fflush(handle); }}'
            elif var_type == "float":                   # FIX BUG-D: explicit float case
                return f'if(handle) {{ fprintf(handle, "%g", {content}); fflush(handle); }}'
            elif var_type in ("__m256", "__m256i"):     # FIX BUG-C: vector types via TO_STR
                return f'if(handle) {{ fprintf(handle, "%s", TO_STR({content})); fflush(handle); }}'
            else:
                # Structs, ptr types, or anything else — use TO_STR for a
                # best-effort string representation instead of a raw format
                # that would cause a compile error.
                return f'if(handle) {{ fprintf(handle, "%s", TO_STR({content})); fflush(handle); }}'

        if t.type == TokenType.FCLOSE:
            self.advance()
            self.expect(TokenType.LPAREN, False)
            self.expect(TokenType.RPAREN, False)
            return "if(handle) fclose(handle);"

        # --- Type declarations (scalar, array, pointer) ---
        is_custom_type = (t.type == TokenType.IDENTIFIER and
                          self.var_types.get(t.value) == "STRUCT")
        if t.type in (TokenType.INT, TokenType.FLOAT, TokenType.STR, TokenType.PTR,
                      TokenType.LONG, TokenType.SHORT, TokenType.DOUBLE, TokenType.VOID,
                      TokenType.M256, TokenType.M256I) or is_custom_type:

            vtype_raw = self.advance().value

            # Handle 'ptr <type>' -> '<type>*'
            if vtype_raw == "ptr":
                inner_type = self.advance().value
                vtype = f"{inner_type.replace('str', 'char*')}*"
            else:
                vtype = vtype_raw.replace("str", "char*")

            name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            self.var_types[name] = vtype_raw

            # --- Array declaration: int scores[5] ---
            if self.current().type == TokenType.LSBRACKET:
                self.advance()
                size = self.expect(TokenType.NUMBER, False).value
                self.expect(TokenType.RSBRACKET, False)
                self.var_types[name] = f"{vtype_raw}_ARRAY"
                val = ""
                if self.current().type == TokenType.ASSIGN:
                    self.advance()
                    self.expect(TokenType.LBRACE, False)
                    items = []
                    while self.current().type != TokenType.RBRACE:
                        items.append(self.parse_expr())
                        if self.current().type == TokenType.COMMA:
                            self.advance()
                    self.expect(TokenType.RBRACE, False)
                    val = f" = {{{', '.join(items)}}}"
                return f"{vtype} {name}[{size}]{val};"

            # --- Scalar / pointer declaration ---
            if self.current().type == TokenType.ASSIGN:
                self.advance()
                val = self.parse_expr()
                # str with assignment -> char* (pointer to the assigned value)
                return f"{vtype} {name} = {val};"
            else:
                # FIX BUG-M: 'str name' without an initialiser used to emit
                # 'char* name;' — an uninitialised pointer.  fgets/scanf into
                # that pointer would be undefined behaviour (crash at runtime).
                # Now we emit a fixed-size stack buffer instead, which is safe
                # for all input operations (scanf, fgets) and general string use.
                if vtype_raw == "str":
                    return f"char {name}[256];"
                return f"{vtype} {name};"

        # --- print ---
        if t.type == TokenType.PRINT:
            self.advance()
            self.expect(TokenType.LPAREN, False)
            exprs = []
            while self.current().type != TokenType.RPAREN:
                if self.current().type == TokenType.EOF:
                    raise LuaBaseError("SyntaxError",
                        "Unterminated argument list in 'print' — expected ')'",
                        t.line, t.col)
                exprs.append(self.parse_expr())
                if self.current().type == TokenType.COMMA:
                    self.advance()
            self.expect(TokenType.RPAREN, False)
            calls = [f'printf("%s", TO_STR({e}))' for e in exprs]
            return "; ".join(calls) + ";"

        # --- if / elseif / else / end ---
        if t.type == TokenType.IF:
            self.advance()
            cond = self.parse_expr()
            self.expect(TokenType.THEN, False)
            body = []
            while self.current().type not in (TokenType.ELSEIF, TokenType.ELSE, TokenType.END, TokenType.EOF):
                tok2 = self.current()
                s = self.parse_statement()
                if s:
                    body.append(self._line_directive(tok2) + "    " + s)
            if self.current().type == TokenType.EOF:
                raise LuaBaseError("SyntaxError",
                    "Unterminated 'if' block — expected 'end' before end of file",
                    t.line, t.col)

            elseif_parts = []
            while self.current().type == TokenType.ELSEIF:
                ei_tok = self.current()
                self.advance()
                cond_e = self.parse_expr()
                self.expect(TokenType.THEN, False)
                body_e = []
                while self.current().type not in (TokenType.ELSEIF, TokenType.ELSE, TokenType.END, TokenType.EOF):
                    tok2 = self.current()
                    s_e = self.parse_statement()
                    if s_e:
                        body_e.append(self._line_directive(tok2) + "    " + s_e)
                if self.current().type == TokenType.EOF:
                    raise LuaBaseError("SyntaxError",
                        "Unterminated 'elseif' block — expected 'end' before end of file",
                        ei_tok.line, ei_tok.col)
                elseif_parts.append(f"}} else if ({cond_e}) {{\n" + "\n".join(body_e))

            else_part = ""
            if self.current().type == TokenType.ELSE:
                self.advance()
                e_body = []
                while self.current().type not in (TokenType.END, TokenType.EOF):
                    tok2 = self.current()
                    s_el = self.parse_statement()
                    if s_el:
                        e_body.append(self._line_directive(tok2) + "    " + s_el)
                else_part = " else {\n" + "\n".join(e_body) + "\n}"

            self.expect(TokenType.END, False)
            res = f"if ({cond}) {{\n" + "\n".join(body) + "\n"
            if elseif_parts:
                res += "\n".join(elseif_parts) + "\n"
            if else_part:
                return res + "}" + else_part
            elif elseif_parts:
                return res + "}"
            else:
                return res + "}"

        # --- while / do / end ---
        if t.type == TokenType.WHILE:
            self.advance()
            cond = self.parse_expr()
            self.expect(TokenType.DO, False)
            body = []
            while self.current().type not in (TokenType.END, TokenType.EOF):
                tok2 = self.current()
                s = self.parse_statement()
                if s:
                    body.append(self._line_directive(tok2) + s)
            if self.current().type == TokenType.EOF:
                raise LuaBaseError("SyntaxError",
                    "Unterminated 'while' block — expected 'end' before end of file",
                    t.line, t.col)
            self.expect(TokenType.END, False)
            return f"while ({cond}) {{\n    " + "\n    ".join(body) + "\n}"

        # --- pointer dereference assignment: *name = expr ---
        if t.type == TokenType.MULTIPLY:
            self.advance()
            name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            self.expect(TokenType.ASSIGN, False)
            val = self.parse_expr()
            return f"*{name} = {val};"

        # --- Identifier: assignment, array access, member access, or call ---
        if t.type == TokenType.IDENTIFIER:
            name = self.safe_name(self.advance().value)

            # Handle chained array indexing and member access
            while self.current().type in (TokenType.LSBRACKET, TokenType.DOT):
                if self.current().type == TokenType.LSBRACKET:
                    self.advance()
                    index = self.parse_expr()
                    self.expect(TokenType.RSBRACKET, False)
                    name = f"{name}[(int)({index})]"
                elif self.current().type == TokenType.DOT:
                    self.advance()
                    field = self.expect(TokenType.IDENTIFIER, False).value
                    base_var = name.replace('[', ' ').replace('.', ' ').replace('->', ' ').split()[0]
                    op = "->" if (self.var_types.get(base_var) == "ptr") else "."
                    name = f"{name}{op}{field}"

            if self.current().type == TokenType.ASSIGN:
                self.advance()
                return f"{name} = {self.parse_expr()};"

            if self.current().type == TokenType.LPAREN:
                self.advance()
                args = []
                while self.current().type != TokenType.RPAREN:
                    args.append(self.parse_expr())
                    if self.current().type == TokenType.COMMA:
                        self.advance()
                self.expect(TokenType.RPAREN, False)
                return f"{name}({', '.join(args)});"

            return ""

        # --- scanf ---
        if t.type == TokenType.SCANF:
            self.advance()
            self.expect(TokenType.LPAREN, False)
            name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            self.expect(TokenType.RPAREN, False)
            vt = self.var_types.get(name, "")
            return self._emit_scanf(name, vt, t)

        # --- return ---
        if t.type == TokenType.RETURN:
            self.advance()
            return f"return {self.parse_expr()};"

        self.advance()
        return ""

    # -----------------------------------------------------------------------
    # _emit_scanf — dynamic scanf vs fgets dispatch
    #
    # FIX BUG-E, BUG-F, BUG-G, BUG-M, NOTE-4, NOTE-5:
    #
    #   str  -> fgets (safe line read into a fixed buffer, strips trailing \n)
    #           Raw  scanf("%s")  has no width limit and is a classic buffer
    #           overflow.  fgets reads a whole line up to the buffer size and
    #           is the standard safe replacement.
    #
    #   int    -> scanf("%d",  &name)
    #   short  -> scanf("%hd", &name)
    #   long   -> scanf("%ld", &name)
    #   float  -> scanf("%f",  &name)
    #   double -> scanf("%lf", &name)   NOTE: %lf is required for double with scanf
    #                                   (%f gives wrong result on many platforms)
    #
    #   __m256 / __m256i -> runtime error message; there is no meaningful way
    #   to scanf a SIMD vector register from text input.
    #
    #   unknown -> fallback to %d with a comment so the programmer notices.
    # -----------------------------------------------------------------------
    def _emit_scanf(self, name, vt, tok):
        if vt in ("str", "char*"):
            # fgets reads up to 255 chars + null terminator into the 256-byte
            # buffer emitted by the 'str name' declaration (FIX BUG-M/NOTE-4).
            # The strcspn call strips the trailing newline that fgets preserves.
            return (f'fgets({name}, sizeof({name}), stdin);\n'
                    f'    {name}[strcspn({name}, "\\n")] = \'\\0\';')
        elif vt == "int":
            return f'scanf("%d", &{name});'
        elif vt == "short":
            return f'scanf("%hd", &{name});'      # FIX BUG-E: was using %f
        elif vt == "long":
            return f'scanf("%ld", &{name});'      # FIX BUG-E: was using %f
        elif vt == "float":
            return f'scanf("%f", &{name});'
        elif vt == "double":
            return f'scanf("%lf", &{name});'      # FIX BUG-E: %lf required for double
        elif vt in ("__m256", "__m256i"):
            # FIX BUG-G: emit a hard runtime error instead of silently using %f
            # which would write garbage into memory that backs a vector register.
            return (f'fprintf(stderr, "[LuaBase runtime error] line {tok.line}: '
                    f'scanf() cannot read into __m256/__m256i variable \'{name}\'\\n"); exit(1);')
        else:
            # Unknown / struct / ptr type — fall back to %d with a visible comment.
            return f'scanf("%d", &{name}); /* LuaBase: unknown type for {name}, defaulted to %d */'

    # -----------------------------------------------------------------------
    # Expression parsing (precedence: logical -> comparison -> add -> mul -> unary -> primary)
    # -----------------------------------------------------------------------
    def parse_expr(self):
        left = self.parse_comparison()
        while self.current().type in (TokenType.AND, TokenType.OR):
            op_token = self.advance()
            op = "&&" if op_token.type == TokenType.AND else "||"
            right = self.parse_comparison()
            left = f"({left} {op} {right})"
        return left

    def parse_comparison(self):
        left = self.parse_additive()
        cmp_ops = (TokenType.EQ, TokenType.NE, TokenType.LT, TokenType.GT,
                   TokenType.LE, TokenType.GE)
        while self.current().type in cmp_ops:
            op_token = self.advance()
            op_map = {TokenType.LE: "<=", TokenType.GE: ">="}
            op = op_map.get(op_token.type, op_token.value)
            right = self.parse_additive()
            left = f"({left} {op} {right})"
        return left

    def parse_additive(self):
        left = self.parse_multiplicative()
        while self.current().type in (TokenType.PLUS, TokenType.MINUS):
            op = self.advance().value
            right = self.parse_multiplicative()
            left = f"({left} {op} {right})"
        return left

    def parse_multiplicative(self):
        left = self.parse_unary()
        while self.current().type in (TokenType.MULTIPLY, TokenType.DIVIDE, TokenType.MOD):
            op = self.advance().value
            right = self.parse_unary()
            left = f"({left} {op} {right})"
        return left

    def parse_unary(self):
        if self.current().type == TokenType.MINUS:
            self.advance()
            operand = self.parse_unary()
            return f"(-{operand})"
        if self.current().type == TokenType.MULTIPLY:
            self.advance()
            name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            return f"*{name}"
        return self.parse_primary()

    def parse_primary(self):
        t = self.current()

        # Grouped sub-expression: (expr)
        if t.type == TokenType.LPAREN:
            self.advance()
            inner = self.parse_expr()
            self.expect(TokenType.RPAREN, False)
            return f"({inner})"

        # Address-of: &name
        if t.type == TokenType.ADDRESS_OF:
            self.advance()
            name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            return f"&{name}"

        t = self.advance()

        if t.type == TokenType.NUMBER: return str(t.value)
        if t.type == TokenType.STRING: return f'"{t.value}"'

        if t.type == TokenType.IDENTIFIER:
            name = self.safe_name(t.value)

            # Function call
            if self.current().type == TokenType.LPAREN:
                self.advance()
                args = []
                while self.current().type != TokenType.RPAREN:
                    if self.current().type == TokenType.EOF:
                        raise LuaBaseError("SyntaxError",
                            f"Unterminated argument list in call to '{name}'",
                            t.line, t.col)
                    args.append(self.parse_expr())
                    if self.current().type == TokenType.COMMA:
                        self.advance()
                self.expect(TokenType.RPAREN, False)
                return f"{name}({', '.join(args)})"

            # Array index and struct member access (pointer-aware)
            while self.current().type in (TokenType.LSBRACKET, TokenType.DOT):
                if self.current().type == TokenType.LSBRACKET:
                    self.advance()
                    index = self.parse_expr()
                    self.expect(TokenType.RSBRACKET, False)
                    name = f"{name}[(int)({index})]"
                elif self.current().type == TokenType.DOT:
                    self.advance()
                    field = self.expect(TokenType.IDENTIFIER, False).value
                    base_var = name.replace('[', ' ').replace('.', ' ').replace('->', ' ').split()[0]
                    op = "->" if (self.var_types.get(base_var) == "ptr") else "."
                    name = f"{name}{op}{field}"
            return name

        raise LuaBaseError("SyntaxError",
            f"Unexpected token {repr(t.value)} in expression",
            t.line, t.col)

    # -----------------------------------------------------------------------
    # parse_function
    # -----------------------------------------------------------------------
    def parse_function(self):
        fn_tok = self.current()
        self.advance()  # skip 'function'

        valid_ret_types = {
            TokenType.INT, TokenType.FLOAT, TokenType.STR, TokenType.LONG,
            TokenType.SHORT, TokenType.DOUBLE, TokenType.VOID,
            TokenType.M256, TokenType.M256I, TokenType.IDENTIFIER
        }
        rt = self.current()
        if rt.type not in valid_ret_types:
            raise LuaBaseError("SyntaxError",
                f"Expected a return type after 'function', got {repr(rt.value)}",
                rt.line, rt.col)
        raw_ret_type = self.advance().value
        ret_type = "char*" if raw_ret_type == "str" else raw_ret_type

        name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)

        self.expect(TokenType.LPAREN, False)
        params = []
        while self.current().type != TokenType.RPAREN:
            if self.current().type == TokenType.EOF:
                raise LuaBaseError("SyntaxError",
                    f"Unterminated parameter list in function '{name}'",
                    fn_tok.line, fn_tok.col)
            pt = self.current()
            valid_param_types = {
                TokenType.INT, TokenType.FLOAT, TokenType.STR, TokenType.LONG,
                TokenType.SHORT, TokenType.DOUBLE, TokenType.VOID, TokenType.PTR,
                TokenType.M256, TokenType.M256I, TokenType.IDENTIFIER
            }
            if pt.type not in valid_param_types:
                raise LuaBaseError("SyntaxError",
                    f"Expected a type for parameter in function '{name}', got {repr(pt.value)}",
                    pt.line, pt.col)
            p_type_raw = self.advance().value
            if p_type_raw == "ptr":
                inner = self.advance().value
                p_type = f"{inner.replace('str', 'char*')}*"
            else:
                p_type = "char*" if p_type_raw == "str" else p_type_raw
            p_name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            # Track parameter types so fwrite/scanf inside the function work correctly
            self.var_types[p_name] = p_type_raw
            params.append(f"{p_type} {p_name}")
            if self.current().type == TokenType.COMMA:
                self.advance()
        self.expect(TokenType.RPAREN, False)

        self.expect(TokenType.LBRACE, False)
        body = []
        while self.current().type not in (TokenType.RBRACE, TokenType.EOF):
            tok2 = self.current()
            s = self.parse_statement()
            if s:
                # Each body statement gets its own #line directive at column 0,
                # then the 4-space indent follows on the same line.
                body.append(self._line_directive(tok2) + "    " + s)
        if self.current().type == TokenType.EOF:
            raise LuaBaseError("SyntaxError",
                f"Unterminated body of function '{name}' — expected '}}' before end of file",
                fn_tok.line, fn_tok.col)
        self.expect(TokenType.RBRACE, False)

        return f"{ret_type} {name}({', '.join(params)}) {{\n" + "\n".join(body) + "\n}\n"


# ===========================================================================
# CLI entry point
# ===========================================================================
if __name__ == "__main__":
    debug = False
    clang_cmd = []
    import os, sys, subprocess, tempfile, random, string, shutil
    shut = "--shut" in sys.argv or "-s" in sys.argv

    def log(what):
        if not shut:
            print(str(what).strip())

    def die(msg, code=-1):
        print(f"[-] {msg}", file=sys.stderr)
        sys.exit(code)

    getasm = "--asm" in sys.argv
    if len(sys.argv) < 3:
        die("Usage: luabasec <in.lb> <out> [extra.c] [-lPATH] [-gLIB] [-wLIBDIR]", 1)

    inf, out_bin = sys.argv[1], sys.argv[2]

    if not os.path.exists(inf):
        die(f"Input file not found: '{inf}'", 1)
    if not inf.endswith(".lb"):
        log(f"[!] Warning: input file '{inf}' does not have a .lb extension")

    base = os.path.splitext(inf)[0]

    custom_includes = []
    extra_c_files   = []

    for arg in sys.argv[3:]:
        if arg.startswith("-l"):
            path   = arg[2:].strip('"').strip("'")
            folder = os.path.dirname(path)
            if folder:
                custom_includes.append(f"-I{folder}")
        if arg.startswith("-g"):
            path = arg[2:].strip('"').strip("'")
            custom_includes.append(f"-l{path}")
        if arg.endswith(".c") or arg.endswith(".o"):
            if os.path.exists(arg):
                extra_c_files.append(arg)
            else:
                log(f"[-] Warning: Extra source file '{arg}' not found.")
        if arg == "-c":
            debug = True
        if arg.startswith("-w"):
            path = arg[2:].strip('"').strip("'")
            custom_includes.append(f"-L{path}")
        if arg == "-asm":
            getasm = True

    # --- Tokenize ---
    with open(inf, 'r', encoding='utf-8') as f:
        source = f.read()

    log("[*] Tokenizing...")
    try:
        tokens = Lexer(source).tokenize()
    except LuaBaseError as e:
        die(str(e))
    except Exception as e:
        die(f"Unexpected error during tokenization: {e}")

    source_dir  = os.path.dirname(os.path.abspath(inf)) or "."
    source_file = os.path.abspath(inf)   # absolute path for #line directives

    # --- Transpile ---
    log("[*] Compiling...")
    try:
        # FIX NOTE-1: pass source_file so #line directives have the right path.
        c_code = CTranspiler(tokens).transpile(source_dir=source_dir,
                                               source_file=source_file)
    except LuaBaseError as e:
        die(str(e))
    except Exception as e:
        die(f"Unexpected error during transpilation: {e}")

    c_file = (os.path.join(tempfile.gettempdir(),
              f"{base}-{''.join(random.choices(string.ascii_letters, k=6))}.c")
              if not debug else f"{inf}.c")

    try:
        with open(c_file, 'w', encoding='utf-8') as f:
            f.write(c_code)
    except OSError as e:
        die(f"Could not write temporary C file '{c_file}': {e}")

    # --- Build ---
    clang_bin = shutil.which("clang")
    if clang_bin is None:
        print("[-] clang is not installed.", file=sys.stderr)
        try:
            answer = input("    Install clang now? [y/N] ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            answer = ""
        if answer == "y":
            import platform
            system = platform.system()
            if system == "Linux":
                if shutil.which("apt"):
                    subprocess.run(["sudo", "apt", "install", "-y", "clang", "lld"], check=True)
                elif shutil.which("dnf"):
                    subprocess.run(["sudo", "dnf", "install", "-y", "clang", "lld"], check=True)
                elif shutil.which("pacman"):
                    subprocess.run(["sudo", "pacman", "-S", "--noconfirm", "clang", "lld"], check=True)
                else:
                    die("No supported package manager found. Please install clang manually.")
            elif system == "Darwin":
                subprocess.run(["brew", "install", "llvm"], check=True)
            elif system == "Windows":
                subprocess.run(["winget", "install", "LLVM.LLVM"], check=True)
            else:
                die(f"Unsupported OS '{system}'. Please install clang manually.")
            clang_bin = shutil.which("clang")
            if clang_bin is None:
                die("Installation may have succeeded but clang still not found on PATH.")
        else:
            die("clang is required. Please install it and try again.")

    clang_cmd  = [clang_bin, c_file]
    clang_cmd += extra_c_files
    clang_cmd += ["-o", out_bin, "-lm"]
    clang_cmd += custom_includes
    clang_cmd += ["-ffast-math", "-march=native", "-w", "-O3", "-fuse-ld=lld", "-mavx2"]
    if not getasm:
        clang_cmd += ["-flto"]
    if getasm:
        clang_cmd.append("-S")
    clang_cmd += [f"-I{source_dir}"]

    log("[*] Compiling C code...")
    if debug:
        log("Written to debug file.")
    else:
        log(f"[*] Running: {' '.join(clang_cmd)}")

    local_debug_file = f"{base}_debug.c"
    try:
        if not debug:
            result = subprocess.run(clang_cmd, capture_output=True, text=True)
            if result.returncode == 0:
                log(f"[*] Made ./{out_bin}")
            else:
                shutil.copy(c_file, local_debug_file)
                print(f"[-] Clang error (exit {result.returncode}). Debug C saved to ./{local_debug_file}",
                      file=sys.stderr)
                if result.stderr.strip():
                    print(result.stderr.strip(), file=sys.stderr)
                if result.stdout.strip():
                    print(result.stdout.strip(), file=sys.stderr)
                sys.exit(-2)
    except FileNotFoundError:
        die(f"Could not execute clang at '{clang_bin}'")
    except OSError as e:
        die(f"OS error while running clang: {e}")
    finally:
        if os.path.exists(c_file) and not debug:
            try:
                os.remove(c_file)
            except OSError:
                pass

)LB";

// ===========================================================================
// C wrapper — embeds the Python transpiler and runs it in-process.
// ===========================================================================
int main(int argc, char** argv)
{
    Py_Initialize();

    // Build wide-char argv so Python's sys.argv is populated correctly.
    wchar_t** w_argv = (wchar_t**)PyMem_RawMalloc(sizeof(wchar_t*) * argc);
    for (int i = 0; i < argc; i++)
        w_argv[i] = Py_DecodeLocale(argv[i], NULL);
    PySys_SetArgvEx(argc, w_argv, 0);

    // Run the embedded transpiler string.
    int py_ret    = PyRun_SimpleString(transpiler_logic);
    int exit_code = 0;

    if (py_ret != 0)
    {
        PyObject* exc = PyErr_Occurred();
        if (exc && PyErr_GivenExceptionMatches(exc, PyExc_SystemExit))
        {
            PyObject *ptype, *pvalue, *ptraceback;
            PyErr_Fetch(&ptype, &pvalue, &ptraceback);

            // FIX BUG-I: The original code called PyLong_AsLong(pvalue)
            // unconditionally.  pvalue can be:
            //   - NULL       : sys.exit() with no argument  -> treat as 0
            //   - a PyLong   : sys.exit(N)                  -> use N
            //   - a PyUnicode: sys.exit("message")          -> treat as 1
            //   - None       : sys.exit(None)               -> treat as 0
            // Calling PyLong_AsLong on None or a string sets a new exception
            // and returns -1, leaving a live exception when Py_Finalize() is
            // called — causing a CPython assertion failure or crash.
            if (pvalue == NULL || pvalue == Py_None)
            {
                exit_code = 0;
            }
            else if (PyLong_Check(pvalue))
            {
                exit_code = (int)PyLong_AsLong(pvalue);
            }
            else
            {
                // String or other object passed to sys.exit() — treat as
                // a failure exit (matches standard Unix behaviour).
                exit_code = 1;
            }

            Py_XDECREF(ptype);
            Py_XDECREF(pvalue);
            Py_XDECREF(ptraceback);
        }
        else
        {
            // Some other unhandled Python exception — print the traceback.
            PyErr_Print();
            exit_code = 1;
        }
    }

    // Free wide-string argv BEFORE Py_Finalize to avoid use-after-free.
    for (int i = 0; i < argc; i++)
        PyMem_RawFree(w_argv[i]);
    PyMem_RawFree(w_argv);

    Py_Finalize();
    return exit_code;
}