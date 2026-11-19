import 'dart:ffi';
import 'dart:io';
import 'package:ffi/ffi.dart';

import 'tinypy_bindings_generated.dart';

const String _libName = 'tinypy';

void flutterPrint(Pointer<Char> text) {
  // ignore: avoid_print
  print(text.cast<Utf8>().toDartString());
}

final TinypyBindings pocket = () {
  DynamicLibrary dylib;
  if (Platform.isMacOS || Platform.isIOS) {
    // dylib = DynamicLibrary.open('$_libName.framework/$_libName');
    dylib = DynamicLibrary.process();
  } else if (Platform.isAndroid || Platform.isLinux) {
    dylib = DynamicLibrary.open('lib$_libName.so');
  } else if (Platform.isWindows) {
    dylib = DynamicLibrary.open('$_libName.dll');
  } else {
    throw UnsupportedError('Unknown platform: ${Platform.operatingSystem}');
  }
  TinypyBindings retval = TinypyBindings(dylib);
  retval.py_initialize();
  retval.py_callbacks().ref.print = Pointer.fromFunction(flutterPrint);
  return retval;
}();
