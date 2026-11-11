# linux_package.cmake - install rules and CPack (deb, rpm) for the Linux packages.
#
# NEVER RUN ON A REAL LINUX BUILD (written 2026-11-11 on a Windows PC with no Linux; see
# docs/feature-records/platform.md). The layout was exercised on Windows with a scratch project and
# dummy targets (FOXSDR_FORCE_LINUX_PACKAGING=ON), which proves the paths and the CPack variables are
# accepted, not that a package installs on a distribution.
#
# THE LAYOUT. Both programs read resources/bandplans and resources/firmware from BESIDE themselves
# (BandPlan::defaultDir and aorFirmwarePath are executable-relative), so the programs and their
# resources live together in <prefix>/lib/foxsdr/ and <prefix>/bin holds two tiny launchers that exec
# them by their real path. (A symlink would also do, /proc/self/exe resolves it; a launcher is the
# one that cannot be broken by a tool that copies the link.)
#
#   <prefix>/bin/foxsdr, foxsdr-engine         launchers (packaging/linux/)
#   <prefix>/lib/foxsdr/cascade, foxsdr-engine the programs
#   <prefix>/lib/foxsdr/resources/{bandplans,firmware}
#   <prefix>/libexec/foxsdr/foxsdr-udev-helper the pkexec helper (core/udev_rules.cpp generates it)
#   <prefix>/share/polkit-1/actions/com.foxsdr.install-udev-rules.policy
#   <prefix>/lib/udev/rules.d/99-foxsdr-sdr.rules   (installed by the package itself)
#   <prefix>/share/applications/foxsdr.desktop, share/icons/hicolor/256x256/apps/foxsdr.png
#
# The Windows install layout is not touched: nothing here runs unless the platform is Linux (or the
# test switch FOXSDR_FORCE_LINUX_PACKAGING is set).
if(NOT ((UNIX AND NOT APPLE) OR FOXSDR_FORCE_LINUX_PACKAGING))
    return()
endif()

set(_FOXSDR_SRC "${CMAKE_CURRENT_LIST_DIR}/..")
set(_FOXSDR_LIB "lib/foxsdr")

if(TARGET foxsdr-engine)
    install(TARGETS foxsdr-engine RUNTIME DESTINATION "${_FOXSDR_LIB}" COMPONENT foxsdr)
    install(PROGRAMS "${_FOXSDR_SRC}/packaging/linux/foxsdr-engine-launcher" RENAME foxsdr-engine
            DESTINATION bin COMPONENT foxsdr)
endif()
if(TARGET cascade)
    install(TARGETS cascade RUNTIME DESTINATION "${_FOXSDR_LIB}" COMPONENT foxsdr)
    install(PROGRAMS "${_FOXSDR_SRC}/packaging/linux/foxsdr-launcher" RENAME foxsdr
            DESTINATION bin COMPONENT foxsdr)
    install(FILES "${_FOXSDR_SRC}/packaging/linux/foxsdr.desktop"
            DESTINATION share/applications COMPONENT foxsdr)
    install(FILES "${_FOXSDR_SRC}/resources/icon/foxsdr-256.png" RENAME foxsdr.png
            DESTINATION share/icons/hicolor/256x256/apps COMPONENT foxsdr)
endif()

install(DIRECTORY "${_FOXSDR_SRC}/resources/bandplans"
        DESTINATION "${_FOXSDR_LIB}/resources" COMPONENT foxsdr FILES_MATCHING PATTERN "*.json")
install(DIRECTORY "${_FOXSDR_SRC}/resources/firmware"
        DESTINATION "${_FOXSDR_LIB}/resources" COMPONENT foxsdr)
install(FILES "${_FOXSDR_SRC}/installer/linux/99-foxsdr-sdr.rules"
        DESTINATION lib/udev/rules.d COMPONENT foxsdr)
install(PROGRAMS "${_FOXSDR_SRC}/packaging/linux/foxsdr-udev-helper"
        DESTINATION libexec/foxsdr COMPONENT foxsdr)
install(FILES "${_FOXSDR_SRC}/packaging/linux/com.foxsdr.install-udev-rules.policy"
        DESTINATION share/polkit-1/actions COMPONENT foxsdr)
install(FILES "${_FOXSDR_SRC}/README.md" "${_FOXSDR_SRC}/LICENSE" "${_FOXSDR_SRC}/PRIVACY.md"
              "${_FOXSDR_SRC}/installer/THIRD-PARTY-LICENSES.txt"
        DESTINATION share/doc/foxsdr COMPONENT foxsdr)

# --- CPack: deb and rpm ------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "foxsdr")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "FoxSDR software-defined-radio receiver")
set(CPACK_PACKAGE_VENDOR "FoxSDR")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://foxsdr.com")
set(CPACK_RESOURCE_FILE_LICENSE "${_FOXSDR_SRC}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_COMPONENTS_ALL foxsdr)
set(CPACK_GENERATOR "DEB;RPM")

# OWNER DECISION: the maintainer line. A placeholder on a reserved domain until the owner names one.
set(CPACK_PACKAGE_CONTACT "FoxSDR <packaging@foxsdr.invalid>")

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_SECTION "hamradio")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)        # needs dpkg-shlibdeps on the build machine (CI installs dpkg-dev)
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "pkexec")  # the one-click USB-rules install; the package ships the rules anyway
set(CPACK_DEB_COMPONENT_INSTALL OFF)

set(CPACK_RPM_PACKAGE_LICENSE "PolyForm-Noncommercial-1.0.0")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Communications")
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
# Directories the filesystem packages already own: claiming them gives "conflicts with filesystem".
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/bin /usr/lib /usr/libexec /usr/share /usr/share/applications /usr/share/doc
    /usr/share/icons /usr/share/icons/hicolor /usr/share/icons/hicolor/256x256
    /usr/share/icons/hicolor/256x256/apps /usr/share/polkit-1 /usr/share/polkit-1/actions
    /usr/lib/udev /usr/lib/udev/rules.d)
set(CPACK_RPM_COMPONENT_INSTALL OFF)

include(CPack)
