import json

# containers, nesting, whitespace
assert json.loads('{}') == {}
assert json.loads('[]') == []
assert json.loads(' \n\t{ "a" : [ 1 , 2 , { "b" : null } ] }\r\n') == {'a': [1, 2, {'b': None}]}
assert json.loads('[[[[[]]]]]') == [[[[[]]]]]
assert json.loads('{"a": 1, "a": 2}') == {'a': 2}
d = json.loads('{"z": 1, "y": 2, "x": 3}')
assert list(d.keys()) == ['z', 'y', 'x']

# literals
assert json.loads('true') is True
assert json.loads('false') is False
assert json.loads('null') is None
nan = json.loads('NaN')
assert nan != nan
assert json.loads('Infinity') == float('inf')
assert json.loads('-Infinity') == float('-inf')

# numbers
assert json.loads('0') == 0
assert json.loads('-0') == 0
assert json.loads('123') == 123
assert json.loads('-123') == -123
assert type(json.loads('7')) is int
assert json.loads('1.5') == 1.5
assert json.loads('-1.5') == -1.5
assert type(json.loads('2.0')) is float
assert json.loads('1e3') == 1000.0
assert json.loads('1E3') == 1000.0
assert json.loads('2.5e-3') == 0.0025
assert json.loads('-2.5E+2') == -250.0
assert json.loads('9223372036854775807') == 9223372036854775807
assert json.loads('[0.1, 0.2, 0.30000000000000004]') == [0.1, 0.2, 0.30000000000000004]

# strings and escapes
assert json.loads('""') == ''
assert json.loads('"plain"') == 'plain'
assert json.loads('"a\\"b"') == 'a"b'
assert json.loads('"a\\\\b"') == 'a\\b'
assert json.loads('"a\\/b"') == 'a/b'
assert json.loads('"\\b\\f\\n\\r\\t"') == '\b\f\n\r\t'
assert json.loads('"\\u0041\\u00e9\\u4e2d"') == 'Aé中'
assert json.loads('"\\ud83d\\ude00"') == '😀'
assert json.loads('"café"') == 'café'
assert json.loads('["x", "\\n", "y"]') == ['x', '\n', 'y']

# round trips
data = {'1': [1, 2, 3] * 10, '2': [1.0, 2.5, -3.25], '3': ['abc', 'd"e', 'f\\g', 'h\ni'],
        '45': {'4': [True, False], '5': [None, None]}, 'empty': {}, 'nested': [[], {}, [[]]]}
assert json.loads(json.dumps(data)) == data
assert json.loads(json.dumps(data, indent=2)) == data

# malformed input still raises
for bad in ['{', '[1, 2', '{"a" 1}', '"unterminated', '', '[1,]x', 'tru']:
    try:
        json.loads(bad)
        exit(1)
    except Exception:
        pass

print('ok')
