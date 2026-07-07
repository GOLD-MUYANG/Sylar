# 重定义目标源码的__FILE__宏，使用相对路径的形式，避免暴露敏感信息

# This function will overwrite the standard predefined macro "__FILE__".
# "__FILE__" expands to the name of the current input file, but cmake
# input the absolute path of source file, any code using the macro 
# would expose sensitive information, such as MORDOR_THROW_EXCEPTION(x),
# so we'd better overwirte it with filename.
function(force_redefine_file_macro_for_sources targetname)
    get_target_property(source_files "${targetname}" SOURCES)
    foreach(sourcefile ${source_files})
        # Get source file's current list of compile definitions.
        get_property(defs SOURCE "${sourcefile}"
            PROPERTY COMPILE_DEFINITIONS)
        # Get the relative path of the source file in project directory
        get_filename_component(filepath "${sourcefile}" ABSOLUTE)
        string(REPLACE ${PROJECT_SOURCE_DIR}/ "" relpath ${filepath})
        list(APPEND defs "__FILE__=\"${relpath}\"")
        # Set the updated compile definitions on the source file.
        set_property(
            SOURCE "${sourcefile}"
            PROPERTY COMPILE_DEFINITIONS ${defs}
            )
    endforeach()
endfunction()

function(ragelmaker src_rl outputlist outputdir)
    #Create a custom build step that will call ragel on the provided src_rl file.
    #The output .cpp file will be appended to the variable name passed in outputlist.

    get_filename_component(src_file ${src_rl} NAME_WE)

    set(rl_out ${outputdir}/${src_file}.rl.cc)

    #adding to the list inside a function takes special care, we cannot use list(APPEND...)
    #because the results are local scope only
    set(${outputlist} ${${outputlist}} ${rl_out} PARENT_SCOPE)

    #Warning: The " -S -M -l -C -T0  --error-format=msvc" are added to match existing window invocation
    #we might want something different for mac and linux
    add_custom_command(
        OUTPUT ${rl_out}
        COMMAND cd ${outputdir}
        COMMAND ragel ${CMAKE_CURRENT_SOURCE_DIR}/${src_rl} -o ${rl_out} -l -C -G2  --error-format=msvc
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src_rl}
        )
    set_source_files_properties(${rl_out} PROPERTIES GENERATED TRUE)
endfunction(ragelmaker)



function(sylar_add_executable targetname srcs depends libs)
    add_executable(${targetname} ${srcs})
    add_dependencies(${targetname} ${depends})
    force_redefine_file_macro_for_sources(${targetname})
    target_link_libraries(${targetname} ${libs})
endfunction()

function(sylar_target_use_jsoncpp targetname)
    target_include_directories(${targetname} PRIVATE ${JSONCPP_INCLUDE_DIRS})
    target_compile_options(${targetname} PRIVATE ${JSONCPP_CFLAGS_OTHER})
endfunction()

function(sylar_add_jsoncpp_executable targetname srcs)
    sylar_add_executable(${targetname} "${srcs}" sylar "${LIBS};${JSONCPP_LIBRARIES}")
    sylar_target_use_jsoncpp(${targetname})
endfunction()

function(sylar_add_test_executable targetname srcs)
    set(depends sylar)
    if(ARGN)
        set(depends ${ARGN})
    endif()
    sylar_add_executable(${targetname} "${srcs}" "${depends}" "${LIBS}")
endfunction()

function(sylar_ctest_name_from_target targetname output_var)
    string(REGEX REPLACE "^test_" "" testname "${targetname}")
    set(${output_var} "${testname}" PARENT_SCOPE)
endfunction()

function(sylar_register_unit_test targetname)
    sylar_ctest_name_from_target(${targetname} testname)
    add_test(NAME ${testname} COMMAND $<TARGET_FILE:${targetname}>)
    set_tests_properties(${testname} PROPERTIES LABELS "unit")
endfunction()

function(sylar_register_unit_command testname)
    add_test(NAME ${testname} COMMAND ${ARGN})
    set_tests_properties(${testname} PROPERTIES LABELS "unit")
endfunction()
