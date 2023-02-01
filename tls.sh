make BUILD_TLS=yes
./utils/gen-test-certs.sh

# https://slproweb.com/products/Win32OpenSSL.html
# https://slproweb.com/download/Win64OpenSSL-3_0_7.msi
# set OpenSSLDir="D:\Program Files\OpenSSL-Win64"
# yum install openssl openssl-devel -y
# sudo apt-get install libssl-dev -y

# make MALLOC=libc
# make MALLOC=jemalloc
# test

#./runtest --tlslibhiredis_ssl.a
#./runtest-cluster --tls

### Running manually
./src/redis-server --tls-port 6379 --port 0 --tls-cert-file ./tests/tls/redis.crt --tls-key-file ./tests/tls/redis.key --tls-ca-cert-file ./tests/tls/ca.crt

./src/redis-cli --tls --cert ./tests/tls/redis.crt --key ./tests/tls/redis.key --cacert ./tests/tls/ca.crt