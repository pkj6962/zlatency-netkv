# znetkv: Zero-Latency Networked Key-Value Store with SPDK and NVMe-oF

> ⚡ A high-performance, ultra-low-latency key-value store designed for disaggregated storage systems, built on SPDK and NVMe-over-Fabrics.

## 🔍 Overview

`znetkv` is a research-grade key-value storage system that enables **direct, user-level access** to remote SSDs via **NVMe-oF** over Ethernet. This system eliminates the traditional I/O bottlenecks caused by the kernel and file system layers.

Key features:

- 🚀 **Zero-latency design** using Intel SPDK (user-level NVMe driver)
- 🌐 **Network-disaggregated architecture** via NVMe over Fabrics (TCP)
- 🧠 **Server-side KV processing** with hash-based indexing
- 📉 **Dynamic load-aware I/O scheduling** across SPDK cores

This repository implements the core architecture described in our MASCOTS '23 paper:  
📄 **[OctoKV: An Agile Network-Based Key-Value Storage System with Robust Load Orchestration](https://doi.org/10.1109/MASCOTS57801.2023.00019)**

## 🧪 Motivation

Traditional KVS on the client side introduces high overhead due to:
- Kernel context switching
- File system address translation
- Interrupt-driven I/O handling

We **offload KV operations to the server**, using SPDK’s user-level polled I/O model, while addressing **load imbalance** between SPDK threads using a custom scheduling framework.

## ⚙️ Architecture

