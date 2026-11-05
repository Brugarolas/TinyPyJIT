# tinypy: portable python 3.x interpreter

<p>
<!-- Build -->
<a title="Build" href="https://github.com/tinypy/tinypy/actions/workflows" >
<img src="https://github.com/tinypy/tinypy/actions/workflows/main.yml/badge.svg" /></a>
<!-- Codecov -->
<a href="https://codecov.io/gh/tinypy/tinypy" > 
<img src="https://codecov.io/gh/tinypy/tinypy/branch/main/graph/badge.svg?token=TI9KAFL0RG"/></a>
<!-- C11 -->
<a href="https://en.wikipedia.org/wiki/C11_(C_standard_revision)">
<img alt="Python" src="https://img.shields.io/badge/C-11-blue.svg"></a>
<!-- License -->
<a href="https://github.com/blueloveth/tinypy/blob/main/LICENSE">
<img alt="GitHub" src="https://img.shields.io/github/license/blueloveth/tinypy.svg?color=blue"></a>
<!-- Github Release -->
<a href="https://github.com/blueloveth/tinypy/releases">
<img alt="GitHub release" src="https://img.shields.io/github/release/blueloveth/tinypy.svg"></a>
<!-- Discord -->
<a title="Discord" href="https://discord.gg/WWaq72GzXv" >
<img src="https://img.shields.io/discord/1048978026131640390.svg" /></a>
<!-- HelloGithub -->
<a href="https://hellogithub.com/repository/dd9c509d72a64caca03d99d5b1991a33" target="_blank"><img src="https://abroad.hellogithub.com/v1/widgets/recommend.svg?rid=dd9c509d72a64caca03d99d5b1991a33&claim_uid=jhOYmWGE75AL0Bp&theme=small" alt="Featured｜HelloGitHub" /></a>
<!-- DeepWiki -->
<a href="https://deepwiki.com/tinypy/tinypy"><img src="https://deepwiki.com/badge.svg" alt="Ask DeepWiki"></a>
</p>

tinypy is a portable Python 3.x interpreter, written in C11.
It aims to be an alternative to Lua for game scripting, with elegant syntax, powerful features and competitive performance.
tinypy has no dependencies other than the C standard library, which can be easily integrated into your C/C++ project.
Developers are able to write Python bindings via C-API or pybind11 compatible interfaces.

Please see https://tinypy.github.io for details and try the following resources.
+ [Live Python Demo](https://tinypy.github.io/static/web/): Run Python code in your browser
+ [Live C Examples](https://tinypy.github.io/examples/): Explore C-APIs in your browser
+ [Godot Extension](https://github.com/tinypy/godot-tinypy): Use tinypy in Godot Engine
+ [VSCode Extension](https://marketplace.visualstudio.com/items?itemName=tinypy.tinypy): Debug and profile tinypy scripts in VSCode
+ [Flutter Plugin](https://pub.dev/packages/tinypy): Use tinypy in Flutter apps
+ [Raylib Bindings](https://github.com/tinypy/raylib-bindings): Use raylib with tinypy

## Supported Platforms

pkpy should work on any platform with a C11 compiler.
These platforms are officially tested.

+ Windows 64-bit
+ Linux 64-bit / 32-bit
+ macOS 64-bit
+ Android 64-bit / 32-bit
+ iOS 64-bit
+ Emscripten 32-bit
+ Raspberry Pi OS 64-bit
+ [Luckfox Pico SDK](https://github.com/LuckfoxTECH/luckfox-pico) 32-bit

On Windows platform, only MSVC compiler is officially supported.

## Quick Start

You have two options to integrate pkpy into your project.

#### Use CMake (Recommended)

Clone the whole repository as a submodule into your project,
In your CMakelists.txt, add the following lines:

```cmake
add_subdirectory(tinypy)
target_link_libraries(<your_target> tinypy)
```

See [CMakeLists.txt](https://github.com/tinypy/tinypy/blob/main/CMakeLists.txt) for details.

It is safe to use `main` branch in production if CI badge is green.

#### Use the single header file

Download the `tinypy.h` and `tinypy.c` on our [GitHub Release](https://github.com/tinypy/tinypy/releases) page.
And `#include` it in your project.

### Compile Flags

To compile it with your project, these flags must be set:

+ `--std=c11` flag must be set
+ For MSVC, `/utf-8` and `/experimental:c11atomics` flag must be set
+ `NDEBUG` macro should be defined for release build, or you will get poor performance

For amalgamated build, run `python amalgamate.py` to generate `tinypy.c` and `tinypy.h` in `amalgamated/` directory.

### Example

```c
#include "tinypy.h"
#include <stdio.h>

static bool int_add(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    PY_CHECK_ARG_TYPE(0, tp_int);
    PY_CHECK_ARG_TYPE(1, tp_int);
    py_i64 a = py_toint(py_arg(0));
    py_i64 b = py_toint(py_arg(1));
    py_newint(py_retval(), a + b);
    return true;
}

int main() {
    // Initialize tinypy
    py_initialize();

    // Hello world!
    bool ok = py_exec("print('Hello world!')", "<string>", EXEC_MODE, NULL);
    if(!ok) goto __ERROR;

    // Create a list: [1, 2, 3]
    py_Ref r0 = py_tmpr0();
    py_newlistn(r0, 3);
    py_newint(py_list_getitem(r0, 0), 1);
    py_newint(py_list_getitem(r0, 1), 2);
    py_newint(py_list_getitem(r0, 2), 3);

    // Eval the sum of the list
    py_Ref f_sum = py_getbuiltin(py_name("sum"));
    py_push(f_sum);
    py_pushnil();
    py_push(r0);
    ok = py_vectorcall(1, 0);
    if(!ok) goto __ERROR;

    printf("Sum of the list: %d\n", (int)py_toint(py_retval()));  // 6

    // Bind native `int_add` as a global variable
    py_newnativefunc(r0, int_add);
    py_setglobal(py_name("add"), r0);

    // Call `add` in python
    ok = py_exec("add(3, 7)", "<string>", EVAL_MODE, NULL);
    if(!ok) goto __ERROR;

    py_i64 res = py_toint(py_retval());
    printf("Sum of 2 variables: %d\n", (int)res);  // 10

    py_finalize();
    return 0;

__ERROR:
    py_printexc();
    py_finalize();
    return 1;
}
```

## Features

| Name            | Example                         | Supported |
| --------------- | ------------------------------- | --------- |
| If Else         | `if..else..elif`                | ✅       |
| Loop            | `for/while/break/continue`      | ✅       |
| Function        | `def f(x,*args,y=1):`           | ✅       |
| Subclass        | `class A(B):`                   | ✅       |
| List            | `[1, 2, 'a']`                   | ✅       |
| ListComp        | `[i for i in range(5)]`         | ✅       |
| Slice           | `a[1:2], a[:2], a[1:]`          | ✅       |
| Tuple           | `(1, 2, 'a')`                   | ✅       |
| Dict            | `{'a': 1, 'b': 2}`              | ✅       |
| F-String        | `f'value is {x}'`               | ✅       |
| Unpacking       | `a, b = 1, 2`                   | ✅       |
| Star Unpacking  | `a, *b = [1, 2, 3]`             | ✅       |
| Exception       | `raise/try..except..`           | ✅       |
| Dynamic Code    | `eval()/exec()`                 | ✅       |
| Reflection      | `hasattr()/getattr()/setattr()` | ✅       |
| Import          | `import/from..import`           | ✅       |
| Context Block   | `with <expr> as <id>:`          | ✅       |
| Type Annotation | `def f(a:int, b:float=1)`       | ✅       |
| Generator       | `yield i`                       | ✅       |
| Decorator       | `@cache`                        | ✅       |
| Match Case      | `match code: case 200:`         | ✅       |

## Used By

|                                                                 | Description                                                              |
|-----------------------------------------------------------------|--------------------------------------------------------------------------|
| [Tesselmax.EM](https://fdtd.io) | Distributed electromagnetic field simulator for silicon photonics and RF/interconnect design.            |
| [godot-tinypy](https://github.com/tinypy/godot-tinypy)    | Godot extension for using tinypy in Godot Engine.                      |
| [TIC-80](https://github.com/nesbox/TIC-80)                      | TIC-80 is a fantasy computer for making, playing and sharing tiny games. |
| [py-js](https://github.com/shakfu/py-js)                        | Python3 externals for Max / MSP.                                         |
| [crescent](https://github.com/chukobyte/crescent)               | Crescent is a cross-platform 2D fighting and beat-em-up game engine.     |
| [orxpy](https://github.com/orx/orx)                             | Python extension for orx engine.                                         |
| [CANopenTerm](https://canopenterm.de/python-api)                | Open-source software tool for CANopen CC networks and devices.           |

Submit a pull request to add your project here.

## Contribution

All kinds of contributions are welcome. Please read [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, build, testing, and PR guidance.

- Submit a Pull Request
  - fix a bug
  - add a new feature
- Open an Issue
  - any suggestions
  - any questions

If you find pkpy useful, consider star this repository.

## Sponsor this project

You can sponsor this project via these ways.

+ [Github Sponsors](https://github.com/sponsors/blueloveTH)
+ [Buy me a coffee](https://www.buymeacoffee.com/blueloveth)

Your sponsorship will help us develop pkpy continuously.

## Premium supporter

Special thanks to our premium supporter, Tesselmax, for sponsoring the development of tinypy.

Tesselmax builds Tesselmax.EM, a distributed electromagnetic field simulator for silicon
photonics and RF/interconnect design (FDTD, FDFD, and mode solving across GPU clusters).
Tesselmax embeds tinypy as the scripting engine in our simulation console, so user scripts drive
geometry, materials, sources, and post-processing.

Website: [https://fdtd.io](https://fdtd.io)

## Reference

+ [cpython](https://github.com/python/cpython)

  The official implementation of Python programming language.

+ [byterun](https://www.aosabook.org/en/500L/a-python-interpreter-written-in-python.html)

  An excellent learning material. It illustrates how Python's virtual machine works.


## Star History

[![Star History Chart](https://star-history.dera.page/svg?repos=tinypy/tinypy&type=Date)](https://star-history.dera.page/#tinypy/tinypy&Date)


## License

[MIT License](http://opensource.org/licenses/MIT)
