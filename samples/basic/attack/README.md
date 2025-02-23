# Attacking enclave memory

This sample demonstrates how memory contents can be easily read when running
in regular Docker but are protected in hardware enclaves.

## Docker (no enclave)

Build the Docker image and run the `read_secret` program in the container:
```sh
docker build -t attackme .
docker run --rm -it attackme /read_secret
# Ready to be attacked...
# Press any key to exit
```

In a separate terminal, run:
```sh
./read_memory.sh read_secret Secret42!
# Saving memory image of process 25465...
# Searching memory for string "Secret42!" in "mem.dump.25465"...
# Secret42!
# Secret42!
# Match found.
```

We were able to read the secret because the memory is unprotected.

## SGX-LKL (hardware enclave)


### Setting Up 

This step in only needed when SGX-LKL is installed the first time. 

To use docker without `sudo` (read this post <https://askubuntu.com/questions/477551/how-can-i-use-docker-without-sudo>)
```sh 
sudo groupadd docker
sudo usermod -aG docker $USER
sudo setfacl -m user:$USER:rw /var/run/docker.sock
```

Install SGX-LKL 
```sh 
sudo -E make install
```

And add SGX-LKL to the search path by editing `~/.bashrc`

```sh 
PATH="/opt/sgx-lkl/bin:$PATH"
```

Then make sure to setup the environment variables for SGX-LKL (read the `README.md` in SGX-LKL homepage)

```sh
sgx-lkl-setup
```

### Building the Image 

Build the SGX-LKL rootfs disk image and run the `read_secret` program inside an enclave:
```sh
/opt/sgx-lkl/bin/sgx-lkl-disk create --docker=attackme --size 5M --encrypt --key-file rootfs.img
SGXLKL_HD_KEY=rootfs.img.key sudo /opt/sgx-lkl/bin/sgx-lkl-run-oe --hw-debug rootfs.img /read_secret

# Ready to be attacked...
# Press any key to exit
```

In a separate terminal, run:
```sh
./read_memory.sh sgx-lkl-run-oe Secret42!
# Saving memory image of process 28272...
# Searching memory for string "Secret42!" in "mem.dump.28272"...
# No match found.
```

We were not able to read the secret because the memory is protected.

Note: You can repeat the experiment and run SGX-LKL with an unprotected
simulation/software enclave with `--sw-debug` and you will be able to
read the secret. Only hardware enclaves provide protection here.
