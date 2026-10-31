#======================== Unit tests configuration ============================#
# Unit tests of Jpeg module. Registered in module CMakeLists.txt by
# UnitTesting_AddPath() and created by UnitTesting_Generate() in "UnitTest"
# build (see EmBi_Platform/CMake/UnitTesting/README.md).

get_filename_component(JPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(MCAL_DIR "${JPEG_DIR}/.." ABSOLUTE)

# The JPEG registers are plain memory (RegMem): the flags of the FIFOs, of the end of the conversion and of the header
# parsing are preset by the tests. Rcc, Nvic, Gpdma and Cache modules are mocked.
UnitTesting_Add_Test(
    NAME            Jpeg
    TEST_SOURCE     Test_Jpeg.c
    UUT_SOURCES     ${JPEG_DIR}/Jpeg.c
                    ${JPEG_DIR}/Jpeg_Reg.c
                    ${JPEG_DIR}/Jpeg_Tables.c
    MOCK_HEADERS    ${MCAL_DIR}/Rcc/Rcc_Port.h
                    ${MCAL_DIR}/Nvic/Nvic_Port.h
                    ${MCAL_DIR}/Gpdma/Gpdma_Port.h
                    ${MCAL_DIR}/Cache/Cache_Port.h
    INCLUDE_DIRS    ${JPEG_DIR}
                    ${MCAL_DIR}/Rcc
                    ${MCAL_DIR}/Nvic
                    ${MCAL_DIR}/Gpdma
                    ${MCAL_DIR}/Cache
    LINK_LIBS       ut_RegMem                           # Registers in host memory
                    ut_CmsisHost                        # CMSIS core intrinsics on host
    FORCE_INCLUDES  ${UNIT_TESTING_CMSIS_HOST_HEADER}   # Replaces cmsis_gcc.h
    HOST_LIBS       Ral_Lib                             # ST LL compiled for host
    COVERAGE_EXCLUDE ".*/Ral/.*"
    RUN_SERIAL
)
