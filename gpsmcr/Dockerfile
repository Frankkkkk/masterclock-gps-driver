FROM ubuntu:disco
MAINTAINER Frank Villaro-Dixon <frank.villaro@infomaniak.ch>


RUN apt-get update && \
apt-get install -y build-essential && \
rm -rf /var/lib/apt/lists/*

COPY src /src
WORKDIR /src
RUN make


FROM ubuntu:disco

WORKDIR /
COPY --from=0 /src/gpsmcr .
COPY --from=0 /src/gmtool .

COPY gpsmcr.conf /etc/
CMD ["./gpsmcr", "-nofork"]

