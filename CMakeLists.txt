cmake_minimum_required(VERSION 3.26)
project(daedalus_cpp VERSION 0.1.0 LANGUAGES C++)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")

# Core: filesystem walk + frontmatter parsing
add_library(dd_core STATIC
  src/core/path.cpp
  src/core/yaml.cpp
)

# Skills subsystem (discovery + registry + skill:// resolver)
add_library(dd_skills STATIC
  src/skills/scanner.cpp
  src/skills/registry.cpp
)
add_dependencies(dd_skills dd_core)

# Harness CLI entrypoint
add_executable(dd src/cli_main.cpp)
add_dependencies(dd dd_skills)

# Test suite
add_executable(dd_tests tests/skills_test.cpp)
add_dependencies(dd_tests dd_skills)
