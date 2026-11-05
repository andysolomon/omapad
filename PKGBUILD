# Build from a checkout of this repository with: makepkg -si
pkgname=omapad
pkgver=1.1.0
pkgrel=1
pkgdesc='Native text editor modelled on Notepad++, built for Omarchy'
arch=('x86_64' 'aarch64')
license=('GPL-3.0-only')
depends=('gtk3')
makedepends=('gcc' 'make' 'pkgconf')
source=()

build() {
  cd "$startdir"
  make BUILD="$srcdir/build" -j"$(nproc)"
}

check() {
  cd "$startdir"
  make BUILD="$srcdir/build" test
}

package() {
  cd "$startdir"
  make BUILD="$srcdir/build" PREFIX=/usr DESTDIR="$pkgdir" install
}
