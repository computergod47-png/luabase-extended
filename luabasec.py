import re
from luabase import run_bytecode

class LuaCompiler:
    def __init__(self):
        self.bytecode = []
        self.variables = {}
        self.label_counter = 0
        self.indent_stack = []  # Track indentation levels for blocks
        self.functions = {}  # Store function metadata
        self.current_function = None  # Track which function we're compiling
    
    def compile(self, source_code):
        """Compile Lua-like code to bytecode"""
        lines = source_code.strip().split('\n')
        self.bytecode = []
        self.label_positions = {}
        self.compile_block(lines, 0)
        self.resolve_labels()
        return self.bytecode
    
    def resolve_labels(self):
        """Resolve all label references to bytecode addresses"""
        # First pass: remove markers and track label positions in the final bytecode
        label_map = {}
        resolved = []
        
        for instr in self.bytecode:
            if "__MARKER__" in instr:
                # Extract label name and record its position in the final bytecode
                label_name = instr.split()[0]
                label_map[label_name] = len(resolved)
            else:
                resolved.append(instr)
        
        # Second pass: replace all label references with their final addresses
        final_bytecode = []
        for instr in resolved:
            # Replace any label references with their addresses
            for label_name, addr in label_map.items():
                if label_name in instr:
                    instr = instr.replace(label_name, str(addr))
            
            final_bytecode.append(instr)
        
        self.bytecode = final_bytecode
    
    def compile_block(self, lines, start_indent):
        """Compile a block of code with proper indentation handling"""
        i = 0
        while i < len(lines):
            line = lines[i]
            stripped = line.lstrip()
            indent = len(line) - len(stripped)
            
            # Skip empty lines and comments
            if not stripped or stripped.startswith('--'):
                i += 1
                continue
            
            # Validate semicolon at end of statement
            if not (stripped.endswith(':') or stripped.endswith(';')):
                raise SyntaxError(f"Line {i + 1}: Statement must end with ';' or ':'\n  {stripped}")
            
            # Remove trailing semicolon if present
            if stripped.endswith(';'):
                stripped = stripped[:-1].strip()
            
            # Stop if we've dedented back to parent level
            if indent < start_indent and start_indent > 0:
                return i
            
            # Skip if indentation is greater than expected (part of a block)
            if indent > start_indent and start_indent > 0:
                i += 1
                continue
            
            # Handle if/elif/else/while/for/function
            if stripped.startswith('function '):
                i = self.compile_function(lines, i)
            elif stripped.startswith('if '):
                i = self.compile_if(lines, i)
            elif stripped.startswith('while '):
                i = self.compile_while(lines, i)
            elif stripped.startswith('for '):
                i = self.compile_for(lines, i)
            else:
                self.compile_line(stripped)
                i += 1
        
        return i
    
    def compile_if(self, lines, start_idx):
        """Compile if/elif/else statements"""
        line = lines[start_idx].lstrip()
        condition = line[3:-1]  # Extract condition from 'if condition:'
        self.compile_expression(condition)
        
        jmp_false = self.create_label()
        self.bytecode.append(f"JMP_IF_FALSE {jmp_false}")
        
        # Compile if body
        current_indent = len(lines[start_idx]) - len(lines[start_idx].lstrip())
        i = start_idx + 1
        while i < len(lines):
            line = lines[i]
            stripped = line.lstrip()
            indent = len(line) - len(stripped)
            
            if not stripped or stripped.startswith('--'):
                i += 1
                continue
            
            if indent <= current_indent:
                break
            
            if stripped.startswith('if ') or stripped.startswith('while ') or stripped.startswith('for '):
                if stripped.startswith('if '):
                    i = self.compile_if(lines, i)
                elif stripped.startswith('while '):
                    i = self.compile_while(lines, i)
                elif stripped.startswith('for '):
                    i = self.compile_for(lines, i)
            else:
                self.compile_line(stripped)
                i += 1
        
        jmp_end = self.create_label()
        self.bytecode.append(f"JMP {jmp_end}")
        self.place_label(jmp_false)
        
        # Check for elif/else
        while i < len(lines):
            line = lines[i]
            stripped = line.lstrip()
            indent = len(line) - len(stripped)
            
            if not stripped or stripped.startswith('--'):
                i += 1
                continue
            
            if indent > current_indent:
                i += 1
                continue
            
            if stripped.startswith('elif '):
                condition = stripped[5:-1]
                self.compile_expression(condition)
                jmp_false = self.create_label()
                self.bytecode.append(f"JMP_IF_FALSE {jmp_false}")
                
                i += 1
                while i < len(lines):
                    line = lines[i]
                    stripped = line.lstrip()
                    indent = len(line) - len(stripped)
                    
                    if not stripped or stripped.startswith('--'):
                        i += 1
                        continue
                    
                    if indent <= current_indent:
                        break
                    
                    self.compile_line(stripped)
                    i += 1
                
                self.bytecode.append(f"JMP {jmp_end}")
                self.place_label(jmp_false)
            
            elif stripped.startswith('else:'):
                i += 1
                while i < len(lines):
                    line = lines[i]
                    stripped = line.lstrip()
                    indent = len(line) - len(stripped)
                    
                    if not stripped or stripped.startswith('--'):
                        i += 1
                        continue
                    
                    if indent <= current_indent:
                        break
                    
                    self.compile_line(stripped)
                    i += 1
                break
            else:
                break
        
        self.place_label(jmp_end)
        return i
    
    def compile_while(self, lines, start_idx):
        """Compile while loops"""
        line = lines[start_idx].lstrip()
        condition = line[6:-1]  # Extract condition from 'while condition:'
        
        loop_start = self.create_label()
        self.place_label(loop_start)
        
        self.compile_expression(condition)
        jmp_end = self.create_label()
        self.bytecode.append(f"JMP_IF_FALSE {jmp_end}")
        
        # Compile loop body
        current_indent = len(lines[start_idx]) - len(lines[start_idx].lstrip())
        i = start_idx + 1
        while i < len(lines):
            line = lines[i]
            stripped = line.lstrip()
            indent = len(line) - len(stripped)
            
            if not stripped or stripped.startswith('--'):
                i += 1
                continue
            
            if indent <= current_indent:
                break
            
            self.compile_line(stripped)
            i += 1
        
        self.bytecode.append(f"JMP {loop_start}")
        self.place_label(jmp_end)
        return i
    
    def compile_for(self, lines, start_idx):
        """Compile for loops - simple counter-based for now"""
        line = lines[start_idx].lstrip()
        # Extract: for var = start, end do
        match = re.match(r'for\s+(\w+)\s*=\s*([^,]+),\s*([^\s]+)', line)
        if not match:
            raise ValueError(f"Invalid for loop syntax: {line}")
        
        var, start_val, end_val = match.groups()
        
        # Initialize counter
        self.compile_expression(start_val)
        heap_addr = f"var_{var}"
        self.variables[var] = heap_addr
        self.bytecode.append(f"STORE_HEAP {heap_addr}")
        
        loop_start = self.create_label()
        self.place_label(loop_start)
        
        # Check condition: var <= end_val
        self.bytecode.append(f"LOAD_HEAP {heap_addr}")
        self.compile_expression(end_val)
        self.bytecode.append("LE")
        
        jmp_end = self.create_label()
        self.bytecode.append(f"JMP_IF_FALSE {jmp_end}")
        
        # Compile loop body
        current_indent = len(lines[start_idx]) - len(lines[start_idx].lstrip())
        i = start_idx + 1
        while i < len(lines):
            line = lines[i]
            stripped = line.lstrip()
            indent = len(line) - len(stripped)
            
            if not stripped or stripped.startswith('--'):
                i += 1
                continue
            
            if indent <= current_indent:
                break
            
            self.compile_line(stripped)
            i += 1
        
        # Increment counter
        self.bytecode.append(f"LOAD_HEAP {heap_addr}")
        self.bytecode.append("PUSH 1")
        self.bytecode.append("ADD")
        self.bytecode.append(f"STORE_HEAP {heap_addr}")
        
        self.bytecode.append(f"JMP {loop_start}")
        self.place_label(jmp_end)
        return i
    
    def compile_function(self, lines, start_idx):
        """Compile function definitions"""
        line = lines[start_idx].lstrip()
        # Extract: function name(param1, param2):
        match = re.match(r'function\s+(\w+)\s*\((.*?)\):', line)
        if not match:
            raise SyntaxError(f"Line {start_idx + 1}: Invalid function definition syntax")
        
        func_name = match.group(1)
        params_str = match.group(2)
        params = [p.strip() for p in params_str.split(',')] if params_str else []
        
        # Store function metadata
        func_start = len(self.bytecode)
        self.functions[func_name] = {
            'params': params,
            'start': func_start
        }
        self.current_function = func_name
        
        # Generate function definition bytecode
        self.bytecode.append(f"DEFINE_FUNCTION {func_name} {','.join(params)}")
        
        # Compile function body
        current_indent = len(lines[start_idx]) - len(lines[start_idx].lstrip())
        i = start_idx + 1
        
        while i < len(lines):
            line = lines[i]
            stripped = line.lstrip()
            indent = len(line) - len(stripped)
            
            if not stripped or stripped.startswith('--'):
                i += 1
                continue
            
            if indent <= current_indent:
                break
            
            if stripped.startswith('return '):
                expr = stripped[7:-1] if stripped.endswith(';') else stripped[7:]
                self.compile_expression(expr)
                self.bytecode.append("RETURN")
                i += 1
                continue
            
            self.compile_line(stripped)
            i += 1
        
        self.bytecode.append("FUNCTION_END")
        self.current_function = None
        return i
    
    def compile_line(self, line):
        """Compile a single line of code"""
        
        # Remove trailing semicolon if present
        if line.endswith(';'):
            line = line[:-1].strip()
        
        # var = input() - input assignment
        if 'input()' in line and '=' in line:
            var = line.split('=')[0].strip()
            heap_addr = f"var_{var}"
            self.variables[var] = heap_addr
            self.bytecode.append("INPUT")
            self.bytecode.append(f"STORE_HEAP {heap_addr}")
        
        # print(expression)
        elif line.startswith('print(') and line.endswith(')'):
            expr = line[6:-1]
            self.compile_expression(expr)
            self.bytecode.append("PRINT")
        
        # var = expression
        elif '=' in line and not any(op in line for op in ['==', '!=', '<=', '>=']):
            var, expr = line.split('=', 1)
            var = var.strip()
            expr = expr.strip()
            
            # Check if it's a function call
            if re.match(r'\w+\(.*\)', expr):
                self.compile_function_call(expr)
            else:
                self.compile_expression(expr)
            
            heap_addr = f"var_{var}"
            self.variables[var] = heap_addr
            self.bytecode.append(f"STORE_HEAP {heap_addr}")
    
    def compile_function_call(self, expr):
        """Compile function calls"""
        # Extract function name and arguments
        match = re.match(r'(\w+)\((.*)\)', expr)
        if not match:
            raise ValueError(f"Invalid function call: {expr}")
        
        func_name = match.group(1)
        args_str = match.group(2)
        args = [a.strip() for a in args_str.split(',')] if args_str else []
        
        # Compile each argument
        for arg in args:
            self.compile_expression(arg)
        
        # Generate CALL instruction
        self.bytecode.append(f"CALL {func_name} {len(args)}")
    
    def compile_expression(self, expr):
        """Compile an expression to bytecode"""
        expr = expr.strip()
        
        # String literal
        if (expr.startswith('"') and expr.endswith('"')) or (expr.startswith("'") and expr.endswith("'")):
            string_val = expr[1:-1]
            self.bytecode.append(f"PUSH_STRING {string_val}")
        
        # Comparison operators
        elif '==' in expr:
            parts = expr.split('==')
            self.compile_expression(parts[0].strip())
            self.compile_expression(parts[1].strip())
            self.bytecode.append("EQ")
        elif '!=' in expr:
            parts = expr.split('!=')
            self.compile_expression(parts[0].strip())
            self.compile_expression(parts[1].strip())
            self.bytecode.append("NEQ")
        elif '<=' in expr:
            parts = expr.split('<=')
            self.compile_expression(parts[0].strip())
            self.compile_expression(parts[1].strip())
            self.bytecode.append("LE")
        elif '>=' in expr:
            parts = expr.split('>=')
            self.compile_expression(parts[0].strip())
            self.compile_expression(parts[1].strip())
            self.bytecode.append("GE")
        elif '<' in expr:
            parts = expr.split('<')
            self.compile_expression(parts[0].strip())
            self.compile_expression(parts[1].strip())
            self.bytecode.append("LT")
        elif '>' in expr:
            parts = expr.split('>')
            self.compile_expression(parts[0].strip())
            self.compile_expression(parts[1].strip())
            self.bytecode.append("GT")
        
        # Arithmetic operators
        elif '+' in expr and not expr.startswith('+'):
            parts = expr.split('+')
            self.compile_expression(parts[0].strip())
            self.compile_expression(parts[1].strip())
            self.bytecode.append("ADD")
        elif '-' in expr and not expr.startswith('-') and expr.count('-') == 1:
            parts = expr.split('-')
            self.compile_expression(parts[0].strip())
            self.compile_expression(parts[1].strip())
            self.bytecode.append("SUB")
        elif '*' in expr:
            parts = expr.split('*')
            self.compile_expression(parts[0].strip())
            self.compile_expression(parts[1].strip())
            self.bytecode.append("MUL")
        elif '/' in expr:
            parts = expr.split('/')
            self.compile_expression(parts[0].strip())
            self.compile_expression(parts[1].strip())
            self.bytecode.append("DIV")
        
        # Function call
        elif re.match(r'\w+\(.*\)', expr):
            self.compile_function_call(expr)
        
        # Variable reference
        elif expr in self.variables:
            heap_addr = self.variables[expr]
            self.bytecode.append(f"LOAD_HEAP {heap_addr}")
        
        # Number literal
        elif expr.isdigit() or (expr.startswith('-') and expr[1:].isdigit()):
            self.bytecode.append(f"PUSH {expr}")
        
        else:
            raise ValueError(f"Unknown expression: {expr}")
    
    def create_label(self):
        """Create a new unique label (bytecode address)"""
        self.label_counter += 1
        return f"__label_{self.label_counter}__"
    
    def place_label(self, label):
        """Place a label marker in bytecode"""
        self.bytecode.append(f"{label} __MARKER__")


def main():
    import sys
    
    if len(sys.argv) < 2:
        print("Usage: py COMPILATION.py <input_file> [output_file]")
        print("Example: py COMPILATION.py CODERS.txt OUTPUT.txt")
        return
    
    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else "OUTPUT.txt"
    
    try:
        # Read code from input file
        with open(input_file, 'r') as f:
            lua_code = f.read()
        
        print(f"Compiling {input_file}...")
        
        compiler = LuaCompiler()
        bytecode = compiler.compile(lua_code)
        
        # Save bytecode to output file
        with open(output_file, 'w') as f:
            for instr in bytecode:
                f.write(instr + '\n')
        
        print(f"Compilation successful! Bytecode saved to {output_file}")
        print(f"Total instructions: {len(bytecode)}")
        
    except FileNotFoundError:
        print(f"Error: File '{input_file}' not found!")
    except SyntaxError as e:
        print(f"Syntax Error: {e}")
    except ValueError as e:
        print(f"Compilation Error: {e}")
    except Exception as e:
        print(f"Unexpected Error: {e}")
# Run it.

if __name__ == "__main__":
    main()
