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
    LPAREN = "LPAREN"; RPAREN = "RPAREN"; LBRACE = "LBRACE"; RBRACE = "RBRACE"
    COMMA = "COMMA"; EOF = "EOF"; LINK = "LINK"; LSBRACKET = "LSBRACKET"; RSBRACKET = "RSBRACKET";
    LONG = "LONG"; DOUBLE = "DOUBLE"; M256 = "M256"; VOID = "VOID"; SHORT = "SHORT"; M256I = "M256I";
    TYPE = "TYPE"; ELSEIF = "ELSEIF"; AND = "AND"; OR = "OR"; PTR = "PTR"; ADDRESS_OF = "ADDRESS_OF";
    DOT = "DOT"

class Token:
    def __init__(self, ttype, value): self.type = ttype; self.value = value

class Lexer:
    def __init__(self, source):
        self.source = source; self.pos = 0
        self.keywords = {
            'if': TokenType.IF, 'then': TokenType.THEN, 'else': TokenType.ELSE,
            'end': TokenType.END, 'while': TokenType.WHILE, 'do': TokenType.DO,
            'function': TokenType.FUNCTION, 'return': TokenType.RETURN,
            'print': TokenType.PRINT, 'scanf': TokenType.SCANF,
            'int': TokenType.INT, 'str': TokenType.STR, 'float': TokenType.FLOAT,
            'fopen': TokenType.FOPEN, 'fwrite': TokenType.FWRITE, 'fclose': TokenType.FCLOSE,
            'hlt': TokenType.HLT, 'link': TokenType.LINK, 'long': TokenType.LONG, 'double' : TokenType.DOUBLE, 
            '__m256': TokenType.M256, 'void' : TokenType.VOID, 'short' : TokenType.SHORT, '__m256i' : TokenType.M256I,
            'type' : TokenType.TYPE, 'elseif' : TokenType.ELSEIF, 'ptr' : TokenType.PTR
        }

    def tokenize(self):
        tokens = []
        while self.pos < len(self.source):
            char = self.source[self.pos]
            if char.isspace(): self.pos += 1; continue
            if char == '#':
                while self.pos < len(self.source) and self.source[self.pos] != '\n': self.pos += 1
                continue
            if char.isalpha() or char == '_':
                start = self.pos
                while self.pos < len(self.source) and (self.source[self.pos].isalnum() or self.source[self.pos] == '_'): self.pos += 1
                val = self.source[start:self.pos]
                tokens.append(Token(self.keywords.get(val, TokenType.IDENTIFIER), val))
            elif char.isdigit():
                start = self.pos
                while self.pos < len(self.source) and (self.source[self.pos].isdigit() or self.source[self.pos] == '.'): self.pos += 1
                tokens.append(Token(TokenType.NUMBER, self.source[start:self.pos]))
            elif char in ('"', "'"):
                q = char; self.pos += 1; start = self.pos
                while self.pos < len(self.source) and self.source[self.pos] != q: self.pos += 1
                val = self.source[start:self.pos]; self.pos += 1
                tokens.append(Token(TokenType.STRING, val))
            else:
                double = self.source[self.pos:self.pos+2]
                ops2 = {'==': TokenType.EQ, '!=': TokenType.NE, '<=': TokenType.LT, '>=': TokenType.GT, '&&' : TokenType.AND, '||' : TokenType.OR}
                if double in ops2:
                    tokens.append(Token(ops2[double], double)); self.pos += 2
                else:
                    ops1 = {'+': TokenType.PLUS, '-': TokenType.MINUS, '*': TokenType.MULTIPLY, '/': TokenType.DIVIDE,
                           '%': TokenType.MOD, '=': TokenType.ASSIGN, '(': TokenType.LPAREN, ')': TokenType.RPAREN,
                           '{': TokenType.LBRACE, '}': TokenType.RBRACE, ',': TokenType.COMMA, '<': TokenType.LT, 
                           '>': TokenType.GT, ']': TokenType.RSBRACKET, '[': TokenType.LSBRACKET, '&': TokenType.ADDRESS_OF,
                           '.' : TokenType.DOT}
                    
                    if char in ops1: 
                        tokens.append(Token(ops1[char], char))
                    self.pos += 1
        tokens.append(Token(TokenType.EOF, None))
        return tokens

class CTranspiler:
    def __init__(self, tokens):
        self.tokens = tokens; self.pos = 0
        self.headers = [
    
    "#include <stdio.h>\n"
    "#include <stdbool.h>\n"
    "#include <stdlib.h>\n"
    "#include <math.h>\n"  
    "#include <immintrin.h>\n"
    "#include <string.h>\n",
    "char _lb_buf[256];\n"
    "static inline char* _lb_s(char* x) { return x; }\n"
    "static inline char* _lb_f(double x) { sprintf(_lb_buf, \"%g\", x); return _lb_buf; }\n"
    "static inline char* _lb_m(__m256 v) { float f[8]; _mm256_storeu_ps(f, v); sprintf(_lb_buf, \"[%g, %g, %g, %g, %g, %g, %g, %g]\", f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]); return _lb_buf; }\n"
    "#define TO_STR(x) _Generic((x), char*: _lb_s, const char*: _lb_s, __m256: _lb_m, float: _lb_f, double: _lb_f, int: _lb_f, default: _lb_f)(x)"
]
        self.functions = []; self.main_body = []; self.var_types = {}

    def current(self): return self.tokens[self.pos]
    def advance(self): t = self.current(); self.pos += 1; return t
    def expect(self, ttype, shut):
        if self.current().type == ttype: return self.advance()
        if not shut:
            raise Exception(f"Syntax Error: Expected {ttype}")
        return None

    def safe_name(self, name):
        c_keywords = {'double', 'int', 'float', 'char', 'while', 'if', 'return', 'break', 'FILE', 'long', 'void', 'short', '__m256', '__m256i'}
        return f"var_{name}" if name in c_keywords else name

    def transpile(self):
        while self.current().type != TokenType.EOF:
            if self.current().type == TokenType.TYPE:
                self.headers.append(self.parse_type_definition())
            if self.current().type == TokenType.FUNCTION: self.functions.append(self.parse_function())
            # LINK
            elif self.current().type == TokenType.LINK:
                self.advance()
                filename_token = self.expect(TokenType.STRING, True)
                if filename_token == None:
                    raise Exception("Syntax Error: 'link' requires a string filename in quotes, e.g., use \"file.h\"")
                self.headers.append(f'#include "{filename_token.value}"')
            else:
                stmt = self.parse_statement()
                if stmt: self.main_body.append(stmt)
            
        res = "\n".join(self.headers) + "\n" + "\n".join(self.functions) + "\n"
        res += "int main(int argc, char** argv) {\n    " + "\n    ".join(self.main_body) + "\n    return 0;\n}"
        return res
    def parse_type_definition(self):
        self.advance() # Skip 'type'
        struct_name = self.expect(TokenType.IDENTIFIER, False).value
    
        self.expect(TokenType.LBRACE, False)
        fields = []
        while self.current().type != TokenType.RBRACE:
            f_type_raw = self.advance().value
            f_type = f_type_raw.replace("str", "char*")
            f_name = self.expect(TokenType.IDENTIFIER, False).value
            fields.append(f"{f_type} {f_name};")
        self.expect(TokenType.RBRACE, False)
    
        # Register this as a valid type for future declarations
        self.var_types[struct_name] = "STRUCT" 
    
        return f"typedef struct {{\n    " + "\n    ".join(fields) + f"\n}} {struct_name};"

    def parse_statement(self):
        t = self.current()
        if t.type == TokenType.TYPE:
            return self.parse_type_definition()
        # --- HLT (Wait ms) --
        if t.type == TokenType.HLT:
            self.advance(); self.expect(TokenType.LPAREN, False)
            ms = self.parse_expr()
            self.expect(TokenType.RPAREN, False)
            return f"usleep(({ms}) * 1000);"

        # --- File I/O (Safe Mode) ---
        if t.type == TokenType.FOPEN:
            self.advance(); self.expect(TokenType.LPAREN, False)
            fname = self.parse_expr(); self.expect(TokenType.COMMA, False); mode = self.parse_expr(); self.expect(TokenType.RPAREN, False)
            return f'FILE* handle = fopen({fname}, {mode});\n    if (handle == NULL) {{ printf("FAIL: %s\\n", {fname}); exit(1); }}'

        if t.type == TokenType.FWRITE:
            self.advance(); self.expect(TokenType.LPAREN, False); content = self.parse_expr(); self.expect(TokenType.RPAREN, False)
            fmt = '"%s"' if '"' in str(content) else '"%g"'
            return f'if(handle) {{ fprintf(handle, {fmt}, {content}); fflush(handle); }}'

        if t.type == TokenType.FCLOSE:
            self.advance(); self.expect(TokenType.LPAREN, False); self.expect(TokenType.RPAREN, False)
            return "if(handle) fclose(handle);"

        
        # --- Standard Logic (Types & Declarations) ---
        is_custom_type = t.type == TokenType.IDENTIFIER and self.var_types.get(t.value) == "STRUCT"
        if t.type in (TokenType.INT, TokenType.FLOAT, TokenType.STR, TokenType.PTR, TokenType.LONG, TokenType.SHORT) or is_custom_type:
            vtype_raw = self.advance().value
            
            # --- Handle Pointer Syntax: ptr int -> int* ---
            if vtype_raw == "ptr":
                inner_type = self.advance().value
                vtype = f"{inner_type.replace('str', 'char*')}*"
            else:
                vtype = vtype_raw.replace("str", "char*")
            
            # FIXED: Removed the double IDENTIFIER expect bug here
            name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            self.var_types[name] = vtype_raw
    
            # --- Array Check (e.g., int scores[5]) ---
            if self.current().type == TokenType.LSBRACKET:
                self.advance() # eat [
                size = self.expect(TokenType.NUMBER, False).value
                self.expect(TokenType.RSBRACKET, False) # eat ]
                self.var_types[name] = f"{vtype_raw}_ARRAY"
                
                val = ""
                if self.current().type == TokenType.ASSIGN:
                    self.advance()
                    self.expect(TokenType.LBRACE, False)
                    items = []
                    while self.current().type != TokenType.RBRACE:
                        items.append(self.parse_expr())
                        if self.current().type == TokenType.COMMA: self.advance()
                    self.expect(TokenType.RBRACE, False)
                    val = f" = {{{', '.join(items)}}}"
                return f"{vtype} {name}[{size}]{val};"
    
            # --- Scalar/Pointer Declaration ---
            val = ""
            if self.current().type == TokenType.ASSIGN:
                self.advance()
                val = f" = {self.parse_expr()}"
            return f"{vtype} {name}{val};"

        if t.type == TokenType.PRINT:
            self.advance(); self.expect(TokenType.LPAREN, False)
            exprs = []
            while self.current().type != TokenType.RPAREN:
                exprs.append(self.parse_expr())
                if self.current().type == TokenType.COMMA: self.advance()
            self.expect(TokenType.RPAREN, False)
            calls = [f'printf("%s", TO_STR({e}))' for e in exprs]
            return "; ".join(calls) + ";"

        if t.type == TokenType.IF:
            self.advance()
            cond = self.parse_expr()
            self.expect(TokenType.THEN, False)
            
            body = []
            while self.current().type not in (TokenType.ELSEIF, TokenType.ELSE, TokenType.END, TokenType.EOF):
                s = self.parse_statement()
                if s: body.append("    " + s)
            
            elseif_parts = []
            while self.current().type == TokenType.ELSEIF:
                self.advance()
                cond_e = self.parse_expr()
                self.expect(TokenType.THEN, False)
                body_e = []
                while self.current().type not in (TokenType.ELSEIF, TokenType.ELSE, TokenType.END, TokenType.EOF):
                    s_e = self.parse_statement()
                    if s_e: body_e.append("    " + s_e)
                elseif_parts.append(f"}} else if ({cond_e}) {{\n" + "\n".join(body_e))

            else_part = ""
            if self.current().type == TokenType.ELSE:
                self.advance()
                e_body = []
                while self.current().type not in (TokenType.END, TokenType.EOF):
                    s_el = self.parse_statement()
                    if s_el: e_body.append("    " + s_el)
                else_part = " else {\n" + "\n".join(e_body)

            self.expect(TokenType.END, False)
            
            res = f"if ({cond}) {{\n" + "\n".join(body) + "\n"
            if elseif_parts:
                res += "\n".join(elseif_parts) + "\n"
            if else_part:
                res += "}" + else_part + "\n"
            return res + "}"

        if t.type == TokenType.WHILE:
            self.advance(); cond = self.parse_expr(); self.expect(TokenType.DO, False)
            body = []
            while self.current().type not in (TokenType.END, TokenType.EOF):
                s = self.parse_statement()
                if s: body.append("    " + s)
            self.expect(TokenType.END, False)
            return f"while ({cond}) {{\n    " + "\n    ".join(body) + "\n}"

        if t.type == TokenType.MULTIPLY:
            self.advance() # eat *
            name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            self.expect(TokenType.ASSIGN, False) # eat =
            val = self.parse_expr()
            return f"*{name} = {val};"

        # --- Identifier Logic (Assignments & Calls) ---
        if t.type == TokenType.IDENTIFIER:
            name = self.safe_name(self.advance().value)
            
            # Handle nested arrays and member access seamlessly (Pointer Aware)
            while self.current().type in (TokenType.LSBRACKET, TokenType.DOT):
                if self.current().type == TokenType.LSBRACKET:
                    self.advance() # eat [
                    index = self.parse_expr()
                    self.expect(TokenType.RSBRACKET, False) # eat ]
                    name = f"{name}[(int)({index})]"
                elif self.current().type == TokenType.DOT:
                    self.advance() # eat .
                    field = self.expect(TokenType.IDENTIFIER, False).value
                    # Base variable extraction for pointer check
                    base_var = name.replace('[', ' ').replace('.', ' ').replace('->', ' ').split()[0]
                    is_ptr = (self.var_types.get(base_var) == "ptr")
                    op = "->" if is_ptr else "."
                    name = f"{name}{op}{field}"
            
            # Assignment: name = expr (handles scalar, array, and struct members)
            if self.current().type == TokenType.ASSIGN:
                self.advance()
                return f"{name} = {self.parse_expr()};"
            
            # Function Call: name(args)
            if self.current().type == TokenType.LPAREN:
                self.advance()
                args = []
                while self.current().type != TokenType.RPAREN:
                    args.append(self.parse_expr())
                    if self.current().type == TokenType.COMMA: self.advance()
                self.expect(TokenType.RPAREN, False)
                return f"{name}({', '.join(args)});"

        if t.type == TokenType.SCANF:
            self.advance(); self.expect(TokenType.LPAREN, False); name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value); self.expect(TokenType.RPAREN, False)
            fmt = "%d" if self.var_types.get(name) == "int" else "%f"
            return f'scanf("{fmt}", &{name});'
            
        if t.type == TokenType.RETURN: self.advance(); return f"return {self.parse_expr()};"
        
        self.advance(); return ""

    def parse_expr(self):
        left = self.parse_comparison()
        while self.current().type in (TokenType.AND, TokenType.OR):
            op_token = self.advance()
            op = "&&" if op_token.type == TokenType.AND else "||"
            right = self.parse_comparison()
            left = f"({left} {op} {right})"
        return left

    def parse_comparison(self):
        left = self.parse_primary()
        ops = (TokenType.PLUS, TokenType.MINUS, TokenType.EQ, TokenType.LT, 
            TokenType.GT, TokenType.MULTIPLY, TokenType.DIVIDE, TokenType.MOD, TokenType.NE)
        while self.current().type in ops:
            op = self.advance().value
            right = self.parse_primary()
            left = f"({left} {op} {right})"
        return left

    def parse_primary(self):
        t = self.current()
        
        if t.type == TokenType.ADDRESS_OF:
            self.advance()
            name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            return f"&{name}"
        
        if t.type == TokenType.AND:
            self.advance() 
            name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            return f"&{name}"

        if t.type == TokenType.MULTIPLY:
            self.advance() 
            name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            return f"*{name}"

        t = self.advance()
        
        if t.type == TokenType.NUMBER: return t.value
        if t.type == TokenType.STRING: return f'"{t.value}"'

        if t.type == TokenType.IDENTIFIER:
            name = self.safe_name(t.value)
            
            # --- CASE A: Function Call: name(args) ---
            if self.current().type == TokenType.LPAREN:
                self.advance() # eat (
                args = []
                while self.current().type != TokenType.RPAREN:
                    args.append(self.parse_expr())
                    if self.current().type == TokenType.COMMA: 
                        self.advance()
                self.expect(TokenType.RPAREN, False) # eat )
                return f"{name}({', '.join(args)})"
            
            # --- CASE B: Array Access & Struct Member Access (Pointer Aware) ---
            while self.current().type in (TokenType.LSBRACKET, TokenType.DOT):
                if self.current().type == TokenType.LSBRACKET:
                    self.advance() # eat [
                    index = self.parse_expr()
                    self.expect(TokenType.RSBRACKET, False) # eat ]
                    # (int) cast ensures i3-1215U handles memory offsets correctly
                    name = f"{name}[(int)({index})]"
                elif self.current().type == TokenType.DOT:
                    self.advance() # eat .
                    field = self.expect(TokenType.IDENTIFIER, False).value
                    # Determine -> or . based on the base variable type
                    base_var = name.replace('[', ' ').replace('.', ' ').replace('->', ' ').split()[0]
                    is_ptr = (self.var_types.get(base_var) == "ptr")
                    op = "->" if is_ptr else "."
                    name = f"{name}{op}{field}"
            return name
            
        return "0"

    def parse_function(self):
        self.advance() # Skip 'function'
        
        raw_ret_type = self.advance().value
        ret_type = "char*" if raw_ret_type == "str" else raw_ret_type
        
        name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
        
        self.expect(TokenType.LPAREN, False)
        params = []
        while self.current().type != TokenType.RPAREN:
            p_type_raw = self.advance().value
            p_type = "char*" if p_type_raw == "str" else p_type_raw
            p_name = self.safe_name(self.expect(TokenType.IDENTIFIER, False).value)
            
            params.append(f"{p_type} {p_name}")
            if self.current().type == TokenType.COMMA: 
                self.advance()
        self.expect(TokenType.RPAREN, False)
        
        self.expect(TokenType.LBRACE, False)
        body = []
        while self.current().type not in (TokenType.RBRACE, TokenType.EOF):
            s = self.parse_statement()
            if s: body.append("    " + s)
        self.expect(TokenType.RBRACE, False)
        
        return f"{ret_type} {name}({', '.join(params)}) {{\n" + "\n".join(body) + "\n}\n"

if __name__ == "__main__":
    debug = False
    clang_cmd = []
    import os, sys, subprocess, tempfile, random, string, shutil
    shut = "--shut" in sys.argv or "-s" in sys.argv
    def log(what):
        if not shut:
            print(str(what).strip())
    getasm = "--asm" in sys.argv
    if len(sys.argv) < 3:
        print("Usage: luabasec.py <in.lb> <out> [extra.c] [-lPATH]")
        sys.exit(1)

    inf, out_bin = sys.argv[1], sys.argv[2]
    base = os.path.splitext(inf)[0]
    
    custom_includes = []
    extra_c_files = []

    # --- 1. Sift through all arguments ---
    for arg in sys.argv[3:]:
        if arg.startswith("-l"):
            path = arg[2:].strip('"').strip("'")
            folder = os.path.dirname(path) or "."
            custom_includes.append(f"-I{folder}")
        if arg.startswith("-g"):
            path = arg[2:].strip('"').strip('"')
            custom_includes.append(f"-l{path}")
        if arg.endswith(".c") or arg.endswith(".o"):
            if os.path.exists(arg):
                extra_c_files.append(arg)
            else:
                log(f"[-] Warning: Extra source file '{arg}' not found.")
        if arg == "-ydebug":
            debug = True
        if arg.startswith("-w"):
            path = arg[2:].strip('"').strip('"')
            custom_includes.append(f"-L{path}")
        if arg == "-asm":
            getasm = True

    # --- 2. Transpilation (Standard Logic) ---
    with open(inf, 'r') as f: source = f.read()
    log("[*] Tokenizing...")
    try:
        tokens = Lexer(source).tokenize()
    except:
        log("Error in compiling luaBase.")
    log("[*] Compiling...")
    try:
        c_code = CTranspiler(tokens).transpile()
    except:
        log("Error in compiling luaBase code.")
    
    c_file = os.path.join(tempfile.gettempdir(), f"{base}-{''.join(random.choices(string.ascii_letters, k=6))}.c") if not debug else f"{sys.argv[1]}.c"
    with open(c_file, 'w') as f: f.write(c_code)
    source = os.path.dirname(os.path.abspath(inf))

    # --- 3. Build the Final Clang Command ---
    clang_cmd = ["clang", c_file]
    clang_cmd.extend(extra_c_files)
    clang_cmd += ["-o", out_bin]
    clang_cmd += ["-lm"]
    clang_cmd.extend(custom_includes)
    
    # Optimization flags
    clang_cmd += ["-ffast-math", "-march=native", "-w", "-O3", "-fuse-ld=lld", "-mavx2"]
    if not getasm:
        clang_cmd += ["-flto"]
    if getasm:
        clang_cmd.append("-S")
    log("[*] Compiling C code...")
    
    if not source:
        source = "."
    clang_cmd += [f"-I{source}"]

    log(f"[*] Running: {' '.join(clang_cmd)}") if not debug else log("Written to debug file.")
    local_debug_file = f"{base}.c"
    try:
        if not debug:
            result = subprocess.run(clang_cmd, check=True, capture_output=shut, text=True)
            if result.returncode == 0:
                log(f"[*] Made ./{out_bin}")
            else:
                local_debug_file = f"{base}_debug.c"
                shutil.copy(c_file, local_debug_file)
        
                log(f"[-] Clang Error! Debug C code saved to ./{local_debug_file}")
                log(result.stderr)
                sys.exit(-2)
        
    except subprocess.CalledProcessError:
        log(f"[-] Error in compiling C. Please check {base}.c File")
        sys.exit(-2)
    finally:
        if os.path.exists(c_file) and not debug:
            os.remove(c_file)
    
)LB";

int main(int argc, char** argv)
{
    Py_Initialize();
    wchar_t** w_argv = (wchar_t**)PyMem_RawMalloc(sizeof(wchar_t*) * argc);
    for (int i = 0; i < argc; i++) {
        w_argv[i] = Py_DecodeLocale(argv[i], NULL);
    }
    PySys_SetArgv(argc, w_argv);

    // 3. Run the Transpiler from RAM
    PyRun_SimpleString(transpiler_logic);

    // 4. Cleanup
    Py_Finalize();
    
    // Free the wide-string arguments
    for (int i = 0; i < argc; i++) PyMem_RawFree(w_argv[i]);
    PyMem_RawFree(w_argv);

    return 0;
}