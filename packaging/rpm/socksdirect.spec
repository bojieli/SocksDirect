%global debug_package %{nil}

Name:           socksdirect
Version:        0.1.0
Release:        1%{?dist}
Summary:        Fast and compatible user-space sockets

License:        Apache-2.0
URL:            https://github.com/bojieli/SocksDirect
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.16
BuildRequires:  gcc-c++
BuildRequires:  rdma-core-devel
BuildRequires:  numactl-devel
BuildRequires:  libmemcached-devel
BuildRequires:  systemd-rpm-macros

Requires:       rdma-core
Requires:       %{name}-monitor = %{version}-%{release}

%description
SocksDirect is a drop-in BSD socket replacement that bypasses the
kernel for both intra-host (shared memory) and inter-host (RDMA)
transports. This package ships libsd.so for use via LD_PRELOAD.

%package monitor
Summary:        SocksDirect monitor daemon
Requires:       rdma-core
%{?systemd_requires}

%description monitor
The trusted daemon that handles control-plane operations
(connection establishment, port allocation, fork bookkeeping).

%package tools
Summary:        SocksDirect operator tools

%description tools
socksdirect-ctl CLI for inspecting and managing the running monitor.

%package devel
Summary:        SocksDirect development headers
Requires:       %{name} = %{version}-%{release}

%description devel
C/C++ headers for embedding SocksDirect primitives.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -DSOCKSDIRECT_WITH_RDMA=ON \
       -DSOCKSDIRECT_WITH_HERD=OFF \
       -DSOCKSDIRECT_BUILD_TESTS=OFF \
       -DSOCKSDIRECT_BUILD_LEGACY=OFF
%cmake_build

%install
%cmake_install
install -d %{buildroot}%{_sysconfdir}/socksdirect
install -m 0644 packaging/socksdirect.conf.example \
        %{buildroot}%{_sysconfdir}/socksdirect/socksdirect.conf
install -d %{buildroot}%{_unitdir}
install -m 0644 packaging/systemd/socksdirect-monitor.service \
        %{buildroot}%{_unitdir}/

%pre monitor
getent group socksdirect >/dev/null || groupadd -r socksdirect
getent passwd socksdirect >/dev/null || \
    useradd -r -g socksdirect -d / -s /sbin/nologin \
            -c "SocksDirect monitor" socksdirect
exit 0

%post monitor
%systemd_post socksdirect-monitor.service

%preun monitor
%systemd_preun socksdirect-monitor.service

%postun monitor
%systemd_postun_with_restart socksdirect-monitor.service

%files
%license LICENSE
%doc README.md
%{_libdir}/libsd.so
%{_libdir}/libsdcommon.so

%files monitor
%{_sbindir}/socksdirect-monitor
%config(noreplace) %{_sysconfdir}/socksdirect/socksdirect.conf
%{_unitdir}/socksdirect-monitor.service

%files tools
%{_bindir}/socksdirect-ctl

%files devel
%{_includedir}/socksdirect/*

%changelog
* Mon May 05 2026 SocksDirect Maintainers - 0.1.0-1
- Initial RPM packaging.
