#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint tinypy.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'tinypy'
  s.version          = '2.2.1'
  s.summary          = 'A new Flutter FFI plugin project.'
  s.description      = <<-DESC
A new Flutter FFI plugin project.
                       DESC
  s.homepage         = 'https://tinypy.github.io'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'blueloveTH' => 'blueloveth@foxmail.com' }

  # This will ensure the source files in Classes/ are included in the native
  # builds of apps using this FFI plugin. Podspec does not support relative
  # paths, so Classes contains a forwarder C file that relatively imports
  # `../src/*` so that the C sources can be shared among all target platforms.
  s.dependency 'Flutter'
  s.platform = :ios, '12.0'

  s.swift_version = '5.0'

  s.source                    = { :path => '.' }
  s.source_files              = 'Classes/**/*'
  s.library                   = 'c'

  # Flutter.framework does not contain a i386 slice.
  s.pod_target_xcconfig       = {
    'DEFINES_MODULE' => 'YES',
    'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386',
    'OTHER_LDFLAGS' => '-force_load ' + __dir__ + '/tinypy/build/tinypy.xcframework/ios-arm64/libtinypy.a',
  }

  s.prepare_command = <<-CMD
  rm -rf tinypy
  git clone --branch v2.2.1 --depth 1 https://github.com/tinypy/tinypy.git
  cd tinypy
  git submodule update --init --recursive --depth 1
  bash build_ios_libs.sh
CMD
end
