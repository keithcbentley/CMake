cmake_minimum_required(VERSION 3.10)
project(ExternalProjectTest NONE)
if(CMAKE_XCODE_BUILD_SYSTEM VERSION_GREATER_EQUAL 12)
  cmake_policy(SET CMP0114 NEW)
endif()

include(ExternalProject)

# Test ExternalProject, especially with checkouts from VCS

if(NOT DEFINED EP_TEST_CVS OR EP_TEST_CVS)
  find_package(CVS)
endif()
if(NOT DEFINED EP_TEST_CVS AND CVS_FOUND AND (UNIX OR NOT "${CVS_EXECUTABLE}" MATCHES "cygwin"))
  set(EP_TEST_CVS 1)
endif()

if(NOT DEFINED EP_TEST_SVN OR EP_TEST_SVN)
  find_package(Subversion)
  if(Subversion_FOUND AND Subversion_VERSION_SVN VERSION_LESS 1.2)
    message(STATUS "No ExternalProject svn tests with svn client less than version 1.2")
    set(Subversion_FOUND 0)
  endif()
  # Only do svn tests in cygwin/cygwin or not-cygwin/not-cygwin arrangements:
  if(Subversion_FOUND AND CMAKE_CURRENT_BINARY_DIR MATCHES "cygdrive/" AND NOT "${Subversion_SVN_EXECUTABLE}" MATCHES "cygwin")
    message(STATUS "No ExternalProject svn tests with non-cygwin svn client in a /cygdrive based build")
    set(Subversion_FOUND 0)
  endif()
endif()
if(NOT DEFINED EP_TEST_SVN AND Subversion_FOUND)
  set(EP_TEST_SVN 1)
endif()

if(NOT DEFINED EP_TEST_GIT OR EP_TEST_GIT)
  find_package(Git)
endif()
if(NOT DEFINED EP_TEST_GIT AND Git_FOUND)
  message(STATUS "GIT_VERSION_STRING='${GIT_VERSION_STRING}'")
  if(NOT "${GIT_VERSION_STRING}" VERSION_LESS 1.6.5)
    set(EP_TEST_GIT 1)
  endif()
endif()

if(NOT DEFINED EP_TEST_HG OR EP_TEST_HG)
  find_package(Hg)
endif()
if(NOT DEFINED EP_TEST_HG AND Hg_FOUND)
  # Check if hg executable is working
  execute_process(COMMAND "${HG_EXECUTABLE}" --version OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE HG_RV)
  if(HG_RV EQUAL 0)
    set(EP_TEST_HG 1)
  endif()
endif()

message(STATUS "EP_TEST_CVS='${EP_TEST_CVS}' CVS_EXECUTABLE='${CVS_EXECUTABLE}'")
message(STATUS "EP_TEST_SVN='${EP_TEST_SVN}' Subversion_SVN_EXECUTABLE='${Subversion_SVN_EXECUTABLE}'")
message(STATUS "EP_TEST_GIT='${EP_TEST_GIT}' GIT_EXECUTABLE='${GIT_EXECUTABLE}'")
message(STATUS "EP_TEST_HG='${EP_TEST_HG}'   HG_EXECUTABLE='${HG_EXECUTABLE}'")

option(ExternalProjectTest_USE_FOLDERS "Enable folder grouping in IDEs." ON)
if(ExternalProjectTest_USE_FOLDERS)
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)
else()
  set_property(GLOBAL PROPERTY USE_FOLDERS OFF)
endif()

set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER
  "CMakePredefinedTargets-in-ExternalProjectTest")

set(base "${CMAKE_BINARY_DIR}/CMakeExternals")
set(binary_base "${base}/Build")
set_property(DIRECTORY PROPERTY EP_BASE ${base})
set_property(DIRECTORY PROPERTY EP_STEP_TARGETS configure build test)

add_custom_target(NonExternalProjectTarget
  COMMAND ${CMAKE_COMMAND} -E echo NonExternalProjectTarget)

# Empty projects that test all the known ExternalProject_Add argument key words:
#
set(proj AAA-TestAlphabetization)
ExternalProject_Add(${proj}
  BUILD_COMMAND ""
  CONFIGURE_COMMAND ""
  DOWNLOAD_COMMAND ""
  INSTALL_COMMAND ""
)

set(proj ZZZ-TestAlphabetization)
ExternalProject_Add(${proj}
  BUILD_COMMAND ""
  CONFIGURE_COMMAND ""
  DOWNLOAD_COMMAND ""
  INSTALL_COMMAND ""
)

set(proj TargetNameSameAsFolder)
ExternalProject_Add(${proj}
  BUILD_COMMAND ""
  CONFIGURE_COMMAND ""
  DOWNLOAD_COMMAND ""
  INSTALL_COMMAND ""
)
set_property(TARGET ${proj} PROPERTY FOLDER "${proj}")

set(proj MinimalNoOpProject)
ExternalProject_Add(${proj}
  BUILD_COMMAND ""
  CONFIGURE_COMMAND ""
  DOWNLOAD_COMMAND ""
  INSTALL_COMMAND ""
)

set(proj EmptyNoOpProject)
ExternalProject_Add(${proj}
  BUILD_COMMAND ""
  CMAKE_ARGS ""
  CONFIGURE_COMMAND ""
  CVS_REPOSITORY ""
  CVS_MODULE ""
  CVS_TAG ""
  DEPENDS "MinimalNoOpProject" NonExternalProjectTarget
  DOWNLOAD_COMMAND ""
  DOWNLOAD_NO_PROGRESS 1
  INSTALL_COMMAND ""
  PATCH_COMMAND ""
  STEP_TARGETS install update
  SVN_REPOSITORY ""
  SVN_REVISION ""
  SVN_USERNAME ""
  SVN_PASSWORD ""
  SVN_TRUST_CERT 1
  TEST_COMMAND ""
  TIMEOUT ""
  URL ""
  URL_MD5 ""
  UPDATE_COMMAND ""
)
set_property(TARGET ${proj} PROPERTY FOLDER "")

set(proj NoExtractLogDownload)
ExternalProject_Add(${proj}
  URL ${CMAKE_CURRENT_SOURCE_DIR}/gitrepo.tgz
  DOWNLOAD_NO_EXTRACT 1
  LOG_DOWNLOAD 1
  BUILD_COMMAND ""
  CONFIGURE_COMMAND ""
  INSTALL_COMMAND ""
)

set(proj NoExtractBool)
ExternalProject_Add(${proj}
  URL ${CMAKE_CURRENT_SOURCE_DIR}/gitrepo.tgz
  DOWNLOAD_NO_EXTRACT 0
  LOG_DOWNLOAD 1
  BUILD_COMMAND ""
  CONFIGURE_COMMAND ""
  INSTALL_COMMAND ""
)

# CVS-based tests:
#
if(EP_TEST_CVS)
  # Unzip/untar the CVS repository in our source folder so that other
  # projects below may use it to test CVS args of ExternalProject_Add
  #
  set(proj SetupLocalCVSRepository)
  set(local_cvs_repo "${CMAKE_CURRENT_BINARY_DIR}/LocalRepositories/CVS")
  ExternalProject_Add(${proj}
    SOURCE_DIR ${local_cvs_repo}
    URL ${CMAKE_CURRENT_SOURCE_DIR}/cvsrepo.tgz
    URL_MD5 287399370738adfe932e036cbe38d5b0
    BUILD_COMMAND ""
    CONFIGURE_COMMAND "${CVS_EXECUTABLE}" --version
    INSTALL_COMMAND ""
  )
  set_property(TARGET ${proj}
    PROPERTY FOLDER "SetupRepos/Local/Deeply/Nested/For/Testing")

  # The MSYS cvs tool interprets "c:/" as a "machine:" name for SSH.
  # Detect the MSYS cvs and convert the repo path to an MSYS path.
  if(WIN32)
    if(EXISTS "${CVS_EXECUTABLE}")
      file(STRINGS "${CVS_EXECUTABLE}" cvs_is_msys LIMIT_COUNT 1 REGEX "[Mm][Ss][Yy][Ss]")
      if(cvs_is_msys)
        message(STATUS "'${CVS_EXECUTABLE}' is from MSYS (contains '${cvs_is_msys}')")
        string(REGEX REPLACE "^([A-Za-z]):" "/\\1" local_cvs_repo "${local_cvs_repo}")
      endif()
    endif()
  endif()

  # CVS by date stamp:
  #
  set(proj TutorialStep1-CVS-20241115)
  ExternalProject_Add(${proj}
    CVS_REPOSITORY ":local:${local_cvs_repo}"
    CVS_MODULE "TutorialStep1"
    CVS_TAG "-D2024-11-15 23:11:00 UTC"
    UPDATE_COMMAND ""
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalCVSRepository"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "CVS")

  # CVS by tag:
  #
  set(proj TutorialStep1-CVS-testtag1)
  ExternalProject_Add(${proj}
    CVS_REPOSITORY ":local:${local_cvs_repo}"
    CVS_MODULE "TutorialStep1"
    CVS_TAG -rtesttag1
    UPDATE_COMMAND ""
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalCVSRepository"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "CVS")

  # Live CVS / HEAD (no CVS_TAG):
  #
  set(proj TutorialStep1-CVS-HEAD)
  ExternalProject_Add(${proj}
    CVS_REPOSITORY ":local:${local_cvs_repo}"
    CVS_MODULE "TutorialStep1"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalCVSRepository"
    DEPENDS "EmptyNoOpProject"
    DEPENDS "TutorialStep1-CVS-20241115"
    DEPENDS "TutorialStep1-CVS-testtag1"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "CVS")
endif()


# SVN-based tests:
#
if(EP_TEST_SVN)
  # Unzip/untar the SVN repository in our source folder so that other
  # projects below may use it to test SVN args of ExternalProject_Add
  #
  set(proj SetupLocalSVNRepository)
  set(local_svn_repo "${CMAKE_CURRENT_BINARY_DIR}/LocalRepositories/SVN")
  set(local_svn_repo_url "file:///${local_svn_repo}/TutorialStep1")
  ExternalProject_Add(${proj}
    SOURCE_DIR ${local_svn_repo}
    URL ${CMAKE_CURRENT_SOURCE_DIR}/svnrepo.tgz
    URL_MD5 0d75c80611c998e36c36f4a9e1e739d0
    BUILD_COMMAND ""
    CONFIGURE_COMMAND "${Subversion_SVN_EXECUTABLE}" --version
    INSTALL_COMMAND ""
  )
  set_property(TARGET ${proj}
    PROPERTY FOLDER "SetupRepos/Local/Deeply/Nested/For/Testing")

  # SVN by date stamp:
  #
  set(proj TutorialStep1-SVN-20241115)
  ExternalProject_Add(${proj}
    SVN_REPOSITORY "${local_svn_repo_url}"
    SVN_REVISION "-r{2024-11-15 23:23:00 +0000}"
    UPDATE_COMMAND ""
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalSVNRepository"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "SVN")

  # SVN by revision number:
  #
  set(proj TutorialStep1-SVN-r3)
  ExternalProject_Add(${proj}
    SVN_REPOSITORY "${local_svn_repo_url}"
    SVN_REVISION "-r3"
    UPDATE_COMMAND ""
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalSVNRepository"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "SVN")

  # Live SVN / trunk (no SVN_REVISION):
  #
  set(proj TutorialStep1-SVN-trunk)
  ExternalProject_Add(${proj}
    SVN_REPOSITORY "${local_svn_repo_url}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalSVNRepository"
    LOG_DOWNLOAD 1
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "SVN")
endif()

if(EP_TEST_GIT)
  set(local_git_repo "../../LocalRepositories/GIT")

  # Unzip/untar the git repository in our source folder so that other
  # projects below may use it to test git args of ExternalProject_Add
  #
  set(proj SetupLocalGITRepository)
  ExternalProject_Add(${proj}
    SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/LocalRepositories/GIT
    URL ${CMAKE_CURRENT_SOURCE_DIR}/gitrepo.tgz
    BUILD_COMMAND ""
    CONFIGURE_COMMAND "${GIT_EXECUTABLE}" --version
    INSTALL_COMMAND ""
  )
  set_property(TARGET ${proj}
    PROPERTY FOLDER "SetupRepos/Local/Deeply/Nested/For/Testing")

  # git by commit id:
  #
  set(proj TutorialStep1-GIT-byhash)
  ExternalProject_Add(${proj}
    GIT_REPOSITORY "${local_git_repo}"
    GIT_TAG 4ed00009457732fc8b6d75f6159bbc384119c3d1 # generated by gitrepo.bash
    UPDATE_COMMAND ""
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalGITRepository"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "GIT")

  # git by explicit branch/tag name:
  #
  set(proj TutorialStep1-GIT-bytag)
  ExternalProject_Add(${proj}
    GIT_REPOSITORY "${local_git_repo}"
    GIT_TAG "origin/master"
    UPDATE_COMMAND ""
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalGITRepository"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "GIT")

  # Live git / master (no GIT_TAG):
  #
  set(proj TutorialStep1-GIT-master)
  ExternalProject_Add(${proj}
    GIT_REPOSITORY "${local_git_repo}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalGITRepository"
    LOG_UPDATE 1
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "GIT")

  # Live git / master (no GIT_TAG), but shallow
  #
  set(proj TutorialStep1-GIT-shallow-master)
  ExternalProject_Add(${proj}
    GIT_REPOSITORY "${local_git_repo}"
    GIT_SHALLOW 1
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalGITRepository"
    LOG_UPDATE 1
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "GIT")

  # Live git / master (no GIT_TAG), but git config flags
  #
  # The `git clone --config` parameter has been introduced in Git 1.7.7
  if(NOT git_version VERSION_LESS 1.7.7)
    set(proj TutorialStep1-GIT-config)
    ExternalProject_Add(${proj}
      GIT_REPOSITORY "${local_git_repo}"
      GIT_CONFIG core.eol=lf
                 core.autocrlf=input
                 "http.extraheader=AUTHORIZATION: bearer --unsupportedOption"
      CMAKE_GENERATOR "${CMAKE_GENERATOR}"
      CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
      INSTALL_COMMAND ""
      DEPENDS "SetupLocalGITRepository"
      LOG_UPDATE 1
    )
    set_property(TARGET ${proj} PROPERTY FOLDER "GIT")
  endif()

  # git by explicit branch/tag with empty submodule list
  #
  set(proj TutorialStep1-GIT-bytag-withsubmodules)
  ExternalProject_Add(${proj}
    GIT_REPOSITORY "${local_git_repo}"
    GIT_TAG "origin/master"
    GIT_SUBMODULES ""
    UPDATE_COMMAND ""
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalGITRepository"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "GIT")

  # Unzip/untar the git repository in our source folder so that other
  # projects below may use it to test git args of ExternalProject_Add
  #
  set(proj SetupLocalGITRepositoryWithSubmodules)
  ExternalProject_Add(${proj}
    SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/LocalRepositories/GIT-with-submodules
    URL ${CMAKE_CURRENT_SOURCE_DIR}/gitrepo-sub.tgz
    BUILD_COMMAND ""
    CONFIGURE_COMMAND "${GIT_EXECUTABLE}" --version
    INSTALL_COMMAND ""
  )
  set_property(TARGET ${proj}
    PROPERTY FOLDER "SetupRepos/Local/Deeply/Nested/For/Testing")

  set(local_git_repo "../../LocalRepositories/GIT-with-submodules")

  set(proj TS1-GIT-all-GIT_SUBMODULES)
  ExternalProject_Add(${proj}
    GIT_REPOSITORY "${local_git_repo}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
               -DWITH_m1:BOOL=ON
               -DWITH_m2:BOOL=ON
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalGITRepository"
            "SetupLocalGITRepositoryWithSubmodules"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "GIT")

  set(proj TS1-GIT-all-GIT_SUBMODULES-via-CMP0097-OLD)
  set(CMAKE_WARN_DEPRECATED FALSE) # we are testing CMP0097 OLD behavior
  cmake_policy(SET CMP0097 OLD)
  unset(CMAKE_WARN_DEPRECATED)
  ExternalProject_Add(${proj}
    GIT_REPOSITORY "${local_git_repo}"
    GIT_SUBMODULES ""
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
               -DWITH_m1:BOOL=ON
               -DWITH_m2:BOOL=ON
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalGITRepository"
            "SetupLocalGITRepositoryWithSubmodules"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "GIT")

  set(proj TS1-GIT-no-GIT_SUBMODULES)
  cmake_policy(SET CMP0097 NEW)
  ExternalProject_Add(${proj}
    GIT_REPOSITORY "${local_git_repo}"
    GIT_SUBMODULES ""
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
               -DWITH_m1:BOOL=OFF
               -DWITH_m2:BOOL=OFF
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalGITRepository"
            "SetupLocalGITRepositoryWithSubmodules"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "GIT")

  set(proj TS1-GIT-some-GIT_SUBMODULES)
  ExternalProject_Add(${proj}
    GIT_REPOSITORY "${local_git_repo}"
    GIT_SUBMODULES "m/m1"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
               -DWITH_m1:BOOL=ON
               -DWITH_m2:BOOL=OFF
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalGITRepository"
            "SetupLocalGITRepositoryWithSubmodules"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "GIT")

  # Unzip/untar the git repository in our source folder so that other
  # projects below may use it to test git args of ExternalProject_Add
  #
  set(proj SetupLocalGITRepositoryWithRecursiveSubmodules)
  ExternalProject_Add(${proj}
          SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/LocalRepositories/GIT-with-recursive-submodules
          URL ${CMAKE_CURRENT_SOURCE_DIR}/gitrepo-sub-rec.tgz
          BUILD_COMMAND ""
          CONFIGURE_COMMAND "${GIT_EXECUTABLE}" --version
          INSTALL_COMMAND ""
          )
  set_property(TARGET ${proj}
          PROPERTY FOLDER "SetupRepos/Local/Deeply/Nested/For/Testing")

  set(local_git_repo "../../LocalRepositories/GIT-with-recursive-submodules")

  set(proj TS1-GIT-RECURSIVE_SUBMODULES-default)
  ExternalProject_Add(${proj}
          GIT_REPOSITORY "${local_git_repo}"
          CMAKE_GENERATOR "${CMAKE_GENERATOR}"
          CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
                     -DWITH_RECURSIVE:BOOL=ON
          BUILD_COMMAND  ""
          INSTALL_COMMAND ""
          DEPENDS "SetupLocalGITRepository"
                  "SetupLocalGITRepositoryWithSubmodules"
                  "SetupLocalGITRepositoryWithRecursiveSubmodules"
          )
  set_property(TARGET ${proj} PROPERTY FOLDER "GIT")

  set(proj TS1-GIT-RECURSIVE_SUBMODULES-exclusive)
  ExternalProject_Add(${proj}
          GIT_REPOSITORY "${local_git_repo}"
          GIT_SUBMODULES_RECURSE TRUE
          CMAKE_GENERATOR "${CMAKE_GENERATOR}"
          CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
                     -DWITH_RECURSIVE:BOOL=ON
          BUILD_COMMAND  ""
          INSTALL_COMMAND ""
          DEPENDS "SetupLocalGITRepository"
                  "SetupLocalGITRepositoryWithSubmodules"
                  "SetupLocalGITRepositoryWithRecursiveSubmodules"
          )
  set_property(TARGET ${proj} PROPERTY FOLDER "GIT")

  set(proj TS1-GIT-RECURSIVE_SUBMODULES-off)
  ExternalProject_Add(${proj}
          GIT_REPOSITORY "${local_git_repo}"
          GIT_SUBMODULES_RECURSE FALSE
          CMAKE_GENERATOR "${CMAKE_GENERATOR}"
          CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
                     -DWITH_RECURSIVE:BOOL=OFF
          BUILD_COMMAND  ""
          INSTALL_COMMAND ""
          DEPENDS "SetupLocalGITRepository"
                  "SetupLocalGITRepositoryWithSubmodules"
                  "SetupLocalGITRepositoryWithRecursiveSubmodules"
          )
  set_property(TARGET ${proj} PROPERTY FOLDER "GIT")

endif()

if(EP_TEST_HG)
  set(local_hg_repo "../../LocalRepositories/HG")

  # Unzip/untar the hg repository in our source folder so that other
  # projects below may use it to test hg args of ExternalProject_Add
  #
  set(proj SetupLocalHGRepository)
  ExternalProject_Add(${proj}
    SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/LocalRepositories/HG
    URL ${CMAKE_CURRENT_SOURCE_DIR}/hgrepo.tgz
    BUILD_COMMAND ""
    CONFIGURE_COMMAND "${HG_EXECUTABLE}" --version
    INSTALL_COMMAND ""
  )
  set_property(TARGET ${proj}
    PROPERTY FOLDER "SetupRepos/Local/Deeply/Nested/For/Testing")


  # hg by commit id:
  #
  set(proj TutorialStep1-HG-byhash)
  ExternalProject_Add(${proj}
    HG_REPOSITORY "${local_hg_repo}"
    HG_TAG fc5a0c915390
    UPDATE_COMMAND ""
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalHGRepository"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "HG")

  # hg by explicit branch/tag name:
  #
  set(proj TutorialStep1-HG-bytag)
  ExternalProject_Add(${proj}
    HG_REPOSITORY "${local_hg_repo}"
    HG_TAG "default"
    UPDATE_COMMAND ""
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    INSTALL_COMMAND ""
    DEPENDS "SetupLocalHGRepository"
  )
  set_property(TARGET ${proj} PROPERTY FOLDER "HG")

  # Live hg / tip (no HG_TAG):
  #
  # Mercurial 2.1 does not distinguish an empty pull from a failed pull,
  # so do not run the test with that version.
  if(NOT "${HG_VERSION_STRING}" STREQUAL "2.1")
    set(proj TutorialStep1-HG-tip)
    ExternalProject_Add(${proj}
      HG_REPOSITORY "${local_hg_repo}"
      CMAKE_GENERATOR "${CMAKE_GENERATOR}"
      CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
      INSTALL_COMMAND ""
      DEPENDS "SetupLocalHGRepository"
      LOG_UPDATE 1
    )
    set_property(TARGET ${proj} PROPERTY FOLDER "HG")
  endif()
endif()


# Test the testable built/installed products:
#
enable_testing()


# Do at least a smoke test of a built executable from each
# project's build directory...
#
# BuildTree tests:
#
if(EP_TEST_CVS)
  add_test(TutorialStep1-CVS-20241115-BuildTreeTest
    "${binary_base}/TutorialStep1-CVS-20241115/Tutorial" 4)

  add_test(TutorialStep1-CVS-testtag1-BuildTreeTest
    "${binary_base}/TutorialStep1-CVS-testtag1/Tutorial" 64)

  add_test(TutorialStep1-CVS-HEAD-BuildTreeTest
    "${binary_base}/TutorialStep1-CVS-HEAD/Tutorial" 81)
endif()

if(EP_TEST_SVN)
  add_test(TutorialStep1-SVN-20241115-BuildTreeTest
    "${binary_base}/TutorialStep1-SVN-20241115/Tutorial" 100)

  add_test(TutorialStep1-SVN-r3-BuildTreeTest
    "${binary_base}/TutorialStep1-SVN-r3/Tutorial" 99)

  add_test(TutorialStep1-SVN-trunk-BuildTreeTest
    "${binary_base}/TutorialStep1-SVN-trunk/Tutorial" 98)
endif()

if(EP_TEST_GIT)
  add_test(TutorialStep1-GIT-byhash
    ${CMAKE_COMMAND} -P "${binary_base}/TutorialStep1-GIT-byhash/example.cmake")

  add_test(TutorialStep1-GIT-bytag
    ${CMAKE_COMMAND} -P "${binary_base}/TutorialStep1-GIT-bytag/example.cmake")

  add_test(TutorialStep1-GIT-bytag-withsubmodules
    ${CMAKE_COMMAND} -P "${binary_base}/TutorialStep1-GIT-bytag-withsubmodules/example.cmake")

  add_test(TutorialStep1-GIT-shallow-master
    ${CMAKE_COMMAND} -P "${binary_base}/TutorialStep1-GIT-shallow-master/example.cmake")

  add_test(TutorialStep1-GIT-master
    ${CMAKE_COMMAND} -P "${binary_base}/TutorialStep1-GIT-master/example.cmake")

  if(NOT git_version VERSION_LESS 1.7.7)
    add_test(TutorialStep1-GIT-config
      ${CMAKE_COMMAND} -P "${binary_base}/TutorialStep1-GIT-config/example.cmake")
  endif()
endif()

# Test if log works when the first arguments of *_COMMAND is "COMMAND".
set(proj ExternalProject-no-log)
set(download_cmd "")
set(patch_cmd "")
set(update_cmd "")
set(configure_cmd "")
set(build_cmd "")
set(install_cmd "")

ExternalProject_Add(${proj}
  DOWNLOAD_COMMAND "${download_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "download"
  PATCH_COMMAND "${patch_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "patch"
  UPDATE_COMMAND "${update_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "update"
  CONFIGURE_COMMAND "${configure_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "configure"
  BUILD_COMMAND "${build_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "build"
  INSTALL_COMMAND "${install_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "install"
  )

set(proj ExternalProject-log)
ExternalProject_Add(${proj}
  DOWNLOAD_COMMAND "${download_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "download"
  PATCH_COMMAND "${patch_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "patch"
  UPDATE_COMMAND "${update_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "update"
  CONFIGURE_COMMAND "${configure_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "configure"
  BUILD_COMMAND "${build_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "build"
  INSTALL_COMMAND "${install_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "install"
  LOG_DOWNLOAD 1
  LOG_PATCH 1
  LOG_UPDATE 1
  LOG_CONFIGURE 1
  LOG_BUILD 1
  LOG_INSTALL 1
  )

set(proj ExternalProject-log-dir)
ExternalProject_Add(${proj}
  DOWNLOAD_COMMAND "${download_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "download"
  PATCH_COMMAND "${patch_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "patch"
  UPDATE_COMMAND "${update_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "update"
  CONFIGURE_COMMAND "${configure_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "configure"
  BUILD_COMMAND "${build_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "build"
  INSTALL_COMMAND "${install_cmd}" COMMAND "${CMAKE_COMMAND}" -E echo "install"
  LOG_MERGED_STDOUTERR 1
  LOG_DIR ${CMAKE_CURRENT_BINARY_DIR}/different_log
  LOG_DOWNLOAD 1
  LOG_PATCH 1
  LOG_UPDATE 1
  LOG_CONFIGURE 1
  LOG_BUILD 1
  LOG_INSTALL 1
  )
