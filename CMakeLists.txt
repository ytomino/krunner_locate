cmake_minimum_required(VERSION 3.16)

project(krunner_locate)
set(CMAKE_PROJECT_VERSION "0.1.0")

set(QT_MIN_VERSION 5.15.0)
set(KF_MIN_VERSION 5.103.0)

find_package(ECM ${KF5_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_MODULE_PATH})

include(QtVersionOption)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDEInstallDirs)
add_custom_target(fetch-translations) # suppress doing git clone
include(KDECMakeSettings)
include(ECMDeprecationSettings)
include(FeatureSummary)

find_package(
	Qt${QT_MAJOR_VERSION} ${QT_MIN_VERSION} REQUIRED CONFIG COMPONENTS Core Gui
)
find_package(
	KF${QT_MAJOR_VERSION} ${KF_MIN_VERSION} REQUIRED COMPONENTS I18n KIO Runner
)
find_package(ICU REQUIRED uc)

ecm_set_disabled_deprecation_versions(
	QT ${QT_MIN_VERSION}
	KF ${KF_MIN_VERSION}
)

add_subdirectory(source)

feature_summary(
	WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES
)
