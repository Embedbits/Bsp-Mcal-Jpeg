#===================== Integration tests configuration ========================#
# Integration tests of Jpeg module on target (STM32H5E / H5F boards). Registered in module
# CMakeLists.txt by IntegrationTesting_AddPath() and created by
# IntegrationTesting_Generate() in "IntegrationTest" build
# (see EmBi_Platform/CMake/IntegrationTesting/README.md).
#
# The test set is independent - target interface (entry point, initialization,
# fault handlers, system reset) is implemented by ItTarget_Jpeg.c and
# all libraries of the firmware are listed here.

# StartUp calls BspMain() - the entry point of the test firmware is provided by
# the test set (BspMain.h, ItTarget_Jpeg.c)
if(NOT TARGET BspMain_Lib)
    add_library(BspMain_Lib INTERFACE)
    target_include_directories(BspMain_Lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

IntegrationTesting_Add_Test(
    NAME            Jpeg
    TEST_SOURCE     ItTest_Jpeg.c
    SOURCES         ItTarget_Jpeg.c     # Target interface of the test set
    LINK_LIBS       StartUp_Lib         # Startup code (calls BspMain)
                    Nvic_Lib            # Fault handlers, system reset (ItTarget)
                    Jpeg_Lib
                    Gpdma_Lib           # DMA transfer mode
                    Cache_Lib           # Data cache maintenance of the DMA mode
                    Rcc_Lib             # Bus clock
    TIMEOUT         60
)
