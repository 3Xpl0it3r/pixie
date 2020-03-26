case node['platform']
when 'mac_os_x'
  include_recipe 'pixielabs::mac_os_x'
  root_group = 'wheel'
else
  include_recipe 'pixielabs::linux'
  root_group = 'root'
end

execute 'install_python_packages' do
  command 'pip3 install flake8 flake8-mypy setuptools yamllint'
end

# pyyaml is needed by clang-tidy
execute 'install_python2_packages' do
  command 'pip install pyyaml'
end

include_recipe 'pixielabs::phabricator'
include_recipe 'pixielabs::nodejs'
include_recipe 'pixielabs::golang'

execute 'install node packages' do
  command %(/opt/node/bin/npm install -g \
            tslint@5.11.0 typescript@3.0.1 yarn@1.22.4 webpack@4.42.0 jshint@2.9.6 jest@23.4.2)
end

directory '/opt/pixielabs' do
  owner 'root'
  group root_group
  mode '0755'
  action :create
end

directory '/opt/pixielabs/bin' do
  owner 'root'
  group root_group
  mode '0755'
  action :create
end

template '/opt/pixielabs/plenv.inc' do
  source 'plenv.inc.erb'
  owner 'root'
  group root_group
  mode '0644'
  action :create
end

template '/opt/pixielabs/bin/tot' do
  source 'tot.erb'
  owner 'root'
  group root_group
  mode '0755'
  action :create
end

remote_file '/opt/pixielabs/bin/bazel' do
  source node['bazel']['download_path']
  mode 0555
  checksum node['bazel']['sha256']
end

remote_file '/opt/pixielabs/bin/kustomize' do
  source node['kustomize']['download_path']
  mode 0755
  checksum node['kustomize']['sha256']
end

remote_file '/opt/pixielabs/bin/sops' do
  source node['sops']['download_path']
  mode 0755
  checksum node['sops']['sha256']
end

ark 'shellcheck' do
  url node['shellcheck']['download_path']
  has_binaries ['shellcheck']
  checksum node['shellcheck']['sha256']
end

remote_file '/opt/pixielabs/bin/prototool' do
  source node['prototool']['download_path']
  mode 0755
  checksum node['prototool']['sha256']
end
