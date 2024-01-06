import sys


def get_c_type(t):
    return {
        'boolean': 'int',
        'cfunction': 'lua_CFunction',
        'integer': 'lua_Integer',
        'string': 'const char*',
        'pointer': 'const void*',
        'number': 'lua_Number',
        'thread': 'lua_State*',
        'lightuserdata': 'void*',
        'userdata': 'void*',
    }.get(t, None)


line_number = 0
indent = ' ' * 2
filename = sys.argv[1] if len(sys.argv) > 1 else "<unnamed>"
register_func_name = sys.argv[2] if len(
    sys.argv) > 2 else "register_lua_functiohs"

functions = []
constants = []

for line_number, line in enumerate(sys.stdin):
    try:
        line_number += 1

        if line.startswith("#pragma register_fn"):
            register_func_name = line.removeprefix(
                "#pragma register_fn").strip()
            continue

        if line.startswith("#pragma gen_const"):
            args = line.removeprefix("#pragma gen_const").strip().split()
            var_type = args[0]
            var_name = args[1]
            var_value = args[2] if len(args) > 2 else var_name

            constants.append((var_type, var_name, var_value))
            continue

        if not line.startswith("#pragma gen_fn"):
            sys.stdout.write(line)
            continue

        args = line.removeprefix("#pragma gen_fn").strip().split()

        def warn(msg):
            sys.stderr.write(f"warn ({filename}:{line_number}): {msg}\n")
        if len(args) == 0:
            warn("no arguments provided to gen_binding #pragma")

        func_name = args[0]
        return_type = None
        if ':' in func_name:
            split = func_name.split(':')
            if len(split) != 2:
                raise ValueError(
                    'invalid return type format, expected: FUNC_NAME:RET_TYPE')
            func_name, return_type = split
        print(f"static int lua_{func_name}(lua_State* L) {{")

        arg_names = []
        for i, func_arg in enumerate(args[1:]):
            c_type = get_c_type(func_arg)
            getfn = f"luaL_check{func_arg}"
            if func_arg.endswith('lightuserdata'):
                getfn = "lua_touserdata"

            if c_type is None:
                raise ValueError(f"invalid type '{func_arg}'")
            arg_name = f'arg_{i + 1}'
            print(
                f'{indent}{c_type} {arg_name} = {getfn}(L, {i + 1});')
            arg_names.append(arg_name)
        func_invoke = f'{func_name}({", ".join(arg_names)})'
        if return_type is not None:
            print(f'{indent}lua_push{return_type}(L, {func_invoke});')
            print(f'{indent}return 1;')
        else:
            print(f'{indent}{func_invoke};')
            print(f'{indent}return 0;')
        print('}')

        functions.append(func_name)
    except Exception as e:
        sys.stderr.write(f'error on line {line_number}: {e}\n')

print(f"void {register_func_name}(lua_State* L) {{")
for func_name in functions:
    print(f"{indent}lua_pushcfunction(L, lua_{func_name});")
    print(f'{indent}lua_setglobal(L, "{func_name}");')
for var_type, var_name, var_value in constants:
    print(f"{indent}lua_push{var_type}(L, {var_value});")
    print(f"{indent}lua_setglobal(L, \"{var_name}\");")
print("}")
