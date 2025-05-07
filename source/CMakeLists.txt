configure_file(krunner_locate.json.in krunner_locate.json)

add_library(
	krunner_locate
	MODULE krunner_locate.cxx query.cxx use_locate.cxx
)

target_compile_definitions(
	krunner_locate
	PRIVATE TRANSLATION_DOMAIN="plasma_runner_locate"
	PRIVATE QT_NO_CAST_FROM_ASCII
)

target_compile_features(
	krunner_locate
	PRIVATE cxx_std_20
)

target_link_libraries(
	krunner_locate
	Qt::Gui
	KF${QT_MAJOR_VERSION}::I18n KF${QT_MAJOR_VERSION}::KIOWidgets
	KF${QT_MAJOR_VERSION}::Runner
	ICU::uc
)

install(
	TARGETS krunner_locate
	DESTINATION ${KDE_INSTALL_QTPLUGINDIR}/kf${QT_MAJOR_VERSION}/krunner/
)

add_executable(
	test_cli
	test_cli.cxx query.cxx use_locate.cxx
)

target_compile_features(
	test_cli
	PRIVATE cxx_std_20
)

target_link_libraries(
	test_cli
	ICU::uc
)

if(
	(DEFINED CMAKE_COMPILER_IS_CLANG OR DEFINED CMAKE_COMPILER_IS_GNUCC)
	AND NOT ${CMAKE_BUILD_TYPE} STREQUAL Debug
)
	target_compile_options(
		test_cli
		PRIVATE -fdata-sections -ffunction-sections
	)
	
	target_link_options(
		test_cli
		PRIVATE -Wl,--gc-sections,--no-export-dynamic,--strip-debug
	)
endif()
