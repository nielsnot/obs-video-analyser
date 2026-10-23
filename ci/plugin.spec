Name: @PLUGIN_NAME_FEDORA@
Version: @VERSION@
Release: @RELEASE@%{?dist}
Summary: Video Analyser scope dock for OBS Studio
License: GPLv2+

Source0: %{name}-%{version}.tar.bz2
Requires: obs-studio >= @OBS_VERSION@
BuildRequires: cmake, gcc, gcc-c++
BuildRequires: obs-studio-devel
BuildRequires: qt6-qtbase-devel qt6-qtbase-private-devel
Obsoletes: @PLUGIN_NAME@ < %{version}

%description
Video Analyser, based on Color Monitor, adds a configurable scope dock with
waveform, vectorscope, histogram, CIE diagram, diamond, false color, zebra and
focus peaking to help color correction.

%prep
%autosetup -p1

%build
%{cmake} -DLINUX_PORTABLE=OFF -DLINUX_RPATH=OFF -DQT_VERSION=6
%{cmake_build}

%install
%{cmake_install}

%files
%{_libdir}/obs-plugins/@PLUGIN_NAME@.so
%{_datadir}/obs/obs-plugins/@PLUGIN_NAME@/
