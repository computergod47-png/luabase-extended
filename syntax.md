# Rules for the v2.0.x luaBase programming language.

#comment
#variables
#you can use static type
int x = 10
#or dynamic type
x = 10
#supports: int,float,boolean,str

#prints
print("hello ", x, "\n")
#does not automatically place \n at the end of prints, you need to manually do it.

#loops and ifs
while 1 == 1 do
  print("Hello World!\n")
end
int i = 0
for i = 0, i < 25, i = i + 1 do
  print("Hello World\n")
end
if 1 == 1 then
  print("Hello World\n")
elseif 1 == 2 then
  print("If you get this your computer is glitched. \n")
else
  print("Hm no.\n")
end

#how to compare: ==, <, >, <=, >=, !=
#MATH
a = 1 + 3
b = -a
c = 82 * 11
d = -c / 21

#imports

#assume math.lb in same directory as main.lb (our current file)
import math
print(mathfunction(2,1,2))

#Raylib Imports

extern from "raylib.h" import *
#i do not know the raylib boilerplate. Please insert it here. I only know some.

InitWindow(800,600, "simple example")
RAYWHITE = Color(245,245,245,255)
while !WindowShouldClose() do
  DrawRectangle(10,10,10,RAYWHITE)
end
CloseWindow()

#user input

input = 0
print("Input: ")
scanf(input)

#input = stdin

#functions

function main()
  print("Hailo! Thank you if you read this far!\n")
end
main() #just call main()

#Syntax changes in v2.2.0:
- Function declaration changed:
`function myfunc(){}` instead of `function myfunc() end`

- C library logic changed:
Add libraries by changing `self.headers=` in the compiler.
Change the `subprocess.run("clang", ...)` to link your libraries
Libraries already hardcoded in the python script:
  stdio.h
  stdlib.h
  time.h
  unistd.h
  math.h

No need for writing `extern` now, just call the function like:
`CreateWindow(800,600,"Title")`
- Can call anything in C you like:
Except for stuff that the compiler thinks that is in luaBase, you can basically use any C feature you want.
Like:
`struct Player {...}`
########################################
#Syntax Changes in 2.3.0:
new keyword added: `link`
`link` is basically the same as `#include`
You need to tell the compiler your include directories with `<compile_command> -l"PATH/TO/INCLUDE/DIRECTORY`
For linking libraries, add: `<compile_command> -g"PATH/TO/LIBRARY"` in your compile command
Also, the `link` `-l` version uses .h files.


####################################
#Syntax/Features Changes in 2.4.3:
new keyord added: `ptr`
when you use `ptr int x;`, it translates to `int* x;` internally. of course, you can make the ptr to any type you want.
new keyword added: `type`
`type` allows you to make custom structs like:
```
type Vec2 {
  float x
  float y
}
#later in code
Vec2 pos;
pos.x = 10;
pos.y = 5;
```

new feature added: arrays
you use them like:
```
int array[5] = {10,20,30,40,50}
array[1] = 2;
print(array[2], " ", array[1], "\n")
#or, better:
printf("%d  %d\n", array[2], array[1])
#will output: 30  2.
```

new variable types added:
-long
-short
-__m256

function declaration changed:
from:
`function myfunc(){}`
to:
`function void myfunc(){}`
or:
`function int myfunc(){}`
or what ever your function's return type is, the formula is:
`function type functionName(){}`
#alrighty byeee






