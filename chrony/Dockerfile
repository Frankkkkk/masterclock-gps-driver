FROM ubuntu:disco
MAINTAINER Frank Villaro-Dixon <frank.villaro@infomaniak.ch>


RUN apt-get update && apt-get install -y chrony
RUN rm -rf /var/lib/apt/lists/*


COPY chrony.conf /etc/chrony/chrony.conf

RUN chronyd --version

#HEALTHCHECK --interval=60s --timeout=5s CMD chronyc tracking > /dev/null
HEALTHCHECK --interval=120s --timeout=5s CMD chronyc sources GPS | grep GPS | grep '*'
EXPOSE 123/udp

COPY entrypoint.sh /

ENTRYPOINT ["/entrypoint.sh"]


