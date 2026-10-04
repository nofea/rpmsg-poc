docker run --rm zephyrprojectrtos/ci:latest bash -c "
sudo dpkg --add-architecture arm64
echo -e 'Types: deb\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble noble-updates noble-backports\nComponents: main universe restricted multiverse\nArchitectures: amd64\n\nTypes: deb\nURIs: http://security.ubuntu.com/ubuntu/\nSuites: noble-security\nComponents: main universe restricted multiverse\nArchitectures: amd64\n\nTypes: deb\nURIs: http://ports.ubuntu.com/ubuntu-ports\nSuites: noble noble-updates noble-backports\nComponents: main universe restricted multiverse\nArchitectures: arm64\n\nTypes: deb\nURIs: http://ports.ubuntu.com/ubuntu-ports\nSuites: noble-security\nComponents: main universe restricted multiverse\nArchitectures: arm64' | sudo tee /etc/apt/sources.list.d/ubuntu.sources
sudo apt-get update
sudo apt-get install -y libsysfs-dev:arm64
"
