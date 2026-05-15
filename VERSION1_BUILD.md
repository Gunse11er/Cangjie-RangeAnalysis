# version1 build lock

This branch contains the range-analysis stage 1, stage 2, and stage 3 work.
Use this file as the reproducible checkout/build note for `version1`.

## Repository layout

Use the four Cangjie repositories as sibling directories:

```text
project/
  cangjie_compiler/
  cangjie_runtime/
  cangjie_stdx/
  cangjie_tools/
```

`cangjie_compiler` is the repository pushed to:

```text
git@gitlab.com:Gunse11er/operatorparallel.git
```

The other three repositories are upstream dependencies and should be checked
out at the exact commits listed below.

## Locked dependency commits

```text
cangjie_runtime  7aa51b2342d1e70ce449e68acd436769467e04d4
cangjie_stdx     becbc92a62fce5a24ccc3a05827eb5879b293581
cangjie_tools    c92878cc3b5487f348ca01df98e6758d8254f075
```

Example checkout:

```bash
mkdir -p ~/project
cd ~/project

git clone -b version1 git@gitlab.com:Gunse11er/operatorparallel.git cangjie_compiler

git clone https://gitcode.com/Cangjie/cangjie_runtime.git
git clone https://gitcode.com/Cangjie/cangjie_stdx.git
git clone https://gitcode.com/Cangjie/cangjie_tools.git

git -C cangjie_runtime checkout 7aa51b2342d1e70ce449e68acd436769467e04d4
git -C cangjie_stdx checkout becbc92a62fce5a24ccc3a05827eb5879b293581
git -C cangjie_tools checkout c92878cc3b5487f348ca01df98e6758d8254f075
```

## Build notes

Do not reuse a copied `build/` directory from another machine or path. CMake
and Ninja cache absolute paths and compiler paths there, which can produce
errors such as missing `/usr/bin/clang++` or stale `/home/lh/...` paths.

Use a clean build generated on the target machine:

```bash
cd ~/project/cangjie_compiler
python3 build.py clean
python3 build.py build -t release
python3 build.py install
```

After install:

```bash
source ./output/envsetup.sh
cjc --version
```

The LLVM used by `cjc` is the LLVM built and installed under this project, for
example:

```text
cangjie_compiler/output/third_party/llvm
```

Do not point CMake at an arbitrary system LLVM installation. Mixing LLVM
headers from one version with LLVM libraries from another version can cause
compile-time API errors or link-time undefined references.

## Range-analysis verification

After building `cjc`, the stage 3 loop precision examples can be run with:

```bash
cd ~/project/cangjie_compiler/range_analysis_stage3/e2e_loop
rm -rf stage3_loop_static_CHIR stage3_loop_static default.cjo output.txt
CANGJIE_HOME=../../output ../../output/bin/cjc main.cj --dump-chir -O2 --output-type=staticlib -o stage3_loop_static
diff -u expected_output.txt output.txt

cd ~/project/cangjie_compiler/range_analysis_stage3/e2e_loop_descending
rm -rf stage3_desc_static_CHIR stage3_desc_static default.cjo output.txt
CANGJIE_HOME=../../output ../../output/bin/cjc main.cj --dump-chir -O2 --output-type=staticlib -o stage3_desc_static
diff -u expected_output.txt output.txt
```

Expected `range_analysis_stage3/e2e_loop/output.txt`:

```text
[0, 3:1]
[1, 4:1]
4
[0, 2:1]
[0, 1:1]
[1, 2:1]
2
3
```

Expected `range_analysis_stage3/e2e_loop_descending/output.txt`:

```text
[1, 3:1]
0
```
