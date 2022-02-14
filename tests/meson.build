gtest_dep = dependency('gtest', main: true, version: '>1.11.0', disabler: true, required: false)
gmock_dep = dependency('gmock', disabler: true, required: false)

if not gtest_dep.found() or not gmock_dep.found()
  cmake = import('cmake')
  opt_var = cmake.subproject_options()

  opt_var.add_cmake_defines({'CMAKE_CXX_FLAGS': '-Wno-pedantic'})
  opt_var.set_override_option('warning_level', '3')
  opt_var.set_override_option('werror', 'true')

  gtest_proj = cmake.subproject('gtest',
				options: opt_var,
				required: true)
    if gtest_proj.found()
        gtest_dep = declare_dependency(
            dependencies: [
                dependency('threads'),
                gtest_proj.dependency('gtest')
            ]
        )
        gmock_dep = gtest_proj.dependency('gmock')
    else
        assert(
            not get_option('tests').enabled(),
            'Googletest is required if tests are enabled'
        )
    endif
endif

test(
    'virtual-media-ut',
    executable(
        'virtual-media-ut',
        [
            'src/main.cpp',
        ],
        dependencies: [
            boost,
            gmock_dep,
            gtest_dep,
            sdbusplus,
        ],
        include_directories: ['../src', 'src'],
        cpp_args : '-DINJECT_MOCKS'
    ),
    timeout: 120,
)
