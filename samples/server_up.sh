#!/bin/sh

# example server up script
# will be executed when server is up

# all key value pairs in ShadowVPN config file will be passed to this script
# as environment variables, except password

# turn on IP forwarding
sysctl -w net.ipv4.ip_forward=1

# turn on NAT over eth0 and VPN
# if you use other interface name that eth0, replace eth0 with it
iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
iptables -A FORWARD -i eth0 -o $intf -m state --state RELATED,ESTABLISHED -j ACCEPT
iptables -A FORWARD -i $intf -o eth0 -j ACCEPT

# turn on MSS fix
# MSS = MTU - TCP header - IP header
mss=$(($mtu - 40))
iptables -t mangle -A FORWARD -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss $mss

echo $0 done
