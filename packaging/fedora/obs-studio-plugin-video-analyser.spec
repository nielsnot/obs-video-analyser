Name: obs-studio-plugin-video-analyser
Version: @VERSION@
Release: @RELEASE@%{?dist}
Summary: Video Analyser plugin for OBS Studio providing a scope dock with vectorscope, waveform, histogram, zebra, false color
License: GPLv2+

Source0: %{name}-%{version}.tar.bz2
BuildRequires: cmake, gcc, gcc-c++
BuildRequires: obs-studio-devel
BuildRequires: qt6-qtbase-devel qt6-qtbase-private-devel

%description
Video Analyser, based on Color Monitor, contains vectorscope, waveform, histogram,
zebra, and false color in a configurable scope dock to help color correction.

%prep
%autosetup -p1
sed -i -e 's/project(obs-video-analyser/project(video-analyser/g' CMakeLists.txt

%build
%{cmake} -DLINUX_PORTABLE=OFF -DLINUX_RPATH=OFF -DQT_VERSION=6
%{cmake_build}

%install
%{cmake_install}

%files
%{_libdir}/obs-plugins/*.so
%{_datadir}/obs/obs-plugins/*/
%license LICENSE
