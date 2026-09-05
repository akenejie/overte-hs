# Overte Headless-Server (Unofficial) (English)

## Concept: should information be bound to physics?

Are you enjoying VR?
Across distance, you can share the same space with people in Tokyo, Osaka, or even overseas. Truly wonderful technology. Among it all, Overte's greatest strength is that it is not "a platform managed by some big company," but rather a space that you manage and own yourself.

But let me ask you one question.

**What does your "room" currently depend on to survive?**

Is it hosted on someone's PC?
If that machine loses power, is the space no longer accessible?
Isn't your VR space's life bound to "physics" in the form of specific hardware?

Information should not, in essence, be bound to a specific physical medium — it should be far more fluid.
If we could create a "room that does not depend on a specific machine"... if one server becomes unavailable and your space revives simply by quickly relaunching it on someone else's PC, then that VR space can be said to live on. Like human memory, as long as just one person carries the information (space) forward, it does not die.

That is why I conceived of making Overte a portable headless-server (HS).

This project aims to escape environmental dependency, starting with the need for installation.
- Consists of a **single executable file**; no installation required.
- **No administrator privileges required**; simply specify a port number and instantly host a VR space on localhost.
- **Fully portable**: all space-related data is saved under the `data/` folder below the current working directory at launch (changeable to any location with `--data <dir>`).

In other words, if you copy this `data/` folder to another machine, you can host exactly the same room anywhere, regardless of OS or CPU.

## Basic usage
The basics: call the program by specifying the port for each role, as follows.

```bash
overte-hs --domain 40102 --audio 40103 --avatar 40104 --entity 40105 --assets 40106 --entity-script 40107 --messages 40108
```

This assigns a role to each specified port (domain: 40102, audio: 40103...) and the VR space starts to beat.

> **You are free to choose your ports.** Even if you set `--domain` to something other than 40102 (e.g. `--domain 50102`), it just works as long as you keep specifying the ports of the other servers that register with that domain (audio 50103...). Port numbers are not saved in any configuration file, so they can be decided freely per machine and per space.

### Connecting from the client (Interface)
The Overte client is built on the assumption that a specific machine and a specific room are linked: hosting multiple rooms on a single machine is not anticipated, the port number is fixed at 40102 or by an environment variable, and the port cannot be changed from within the GUI. To connect to a room hosted by Overte HS, a little trick is therefore required on the client side.
Specifically, launch the client with the `HIFI_DOMAIN_SERVER_PORT` environment variable temporarily set to the same value as the server's domain port.
```bash
# If the server was started on 50102, specify the same port on the client
HIFI_DOMAIN_SERVER_PORT=50102 ./Overte.AppImage
```
```bat
set HIFI_DOMAIN_SERVER_PORT=50102 & "C:\Program Files\Overte\interface.exe"
```
After launch, press `Ctrl+L` inside the Interface, enter any host name and connect (the port is taken automatically from the environment variable).

By the way, when the environment variable is not set, the Overte client hardcodes `40102` as the default port. If you set `--domain` to anything other than 40102, make sure `HIFI_DOMAIN_SERVER_PORT` matches on the client side too. If you forget this, the client keeps sending check-ins to 40102 and the connection is never established.
(Incidentally, when Akenejie hosted the server locally and entered the room by pressing `Ctrl+L` and specifying the port with `hifi://localhost:50102`, it connected, was disconnected after about 2 seconds, reconnected after another 2 seconds or so, and repeated that loop.)

### Space separation and distribution
Within the generated folder (default `data/`), the role of each server is clearly divided.

| Server role | Storage location inside the data folder |
| --- | --- |
| domain | `config.json` (settings only) |
| entity | `entities/` (space data) |
| asset | `assets/` |

Each server manages its own data autonomously. The domain-server holds none of the entity-server's data and coordinates purely over the network. This makes each server independently copyable, movable and restorable.

For example, copy only a specific data folder (here `assets/`) to another machine and run the following:
```
overte-hs --host 192.168.1.5:40102 --assets 40106
```

This enables distributed configurations such as "the origin Domain server lives on `192.168.1.5:40102`, but only the room's assets function is being processed (hosted) here on this machine right now".

## Eliminating environmental dependency
In upstream Overte, "one room per machine" is apparently the principle: when the server starts, it writes `HIFI_DOMAIN_SERVER_PORT` into the machine's environment variables. Overte HS does not write environment variables, because hosting different rooms on the same machine using different ports would interfere with each other. When hosting multiple spaces on a single machine, do it like this:
```bash
# Space A: ports in the 40102 range
overte-hs --data ./roomA --domain 40102 --audio 40103 --avatar 40104 --entity 40105 --assets 40106 --entity-script 40107 --messages 40108
# Space B: ports in the 50102 range (client launched in a separate window with HIFI_DOMAIN_SERVER_PORT=50102)
overte-hs --data ./roomB --domain 50102 --audio 50103 --avatar 50104 --entity 50105 --assets 50106 --entity-script 50107 --messages 50108
```
Only the client connecting to Space B needs to be started with `HIFI_DOMAIN_SERVER_PORT=50102`, so the two spaces do not interfere with each other.

### Connecting to a room over the Internet
Overte HS does not include the centralized server registration feature that upstream implements; the only communication Overte HS performs is to send and receive with clients that access the ports listening on that machine. To communicate over the Internet, preparations that Overte HS does not support — such as a global IP or port forwarding — are required.
Incidentally, Akenejie uses tunnneji-tail, made as a Tailscale client. With tunnneji-tail, you can share specific ports with specific people only and with a password, so you can create rooms that only certain people can enter. If the person you shared with NATs to 40102, the temporary environment variable is no longer needed.

## License

The modifications from upstream in this project are licensed under the **GNU Affero General Public License v3.0 (AGPLv3)**.

* **Upstream code**: This project is based on [Overte](https://github.com/overte-org/overte). The upstream code remains under the Apache License 2.0 (`LICENSE`).
* **Modified portions**: The copyright of the fork's modifications, such as the headless conversion and decentralization, belongs to Akenejie, and they are provided under AGPLv3 (`LICENSE-AGPL-3.0.txt`). See `NOTICE` for details.

Under section 13 of the AGPLv3, when this software (or a modified version of it) is made available to users over a network, you are obliged to make the complete corresponding source code (Corresponding Source), including modifications, freely obtainable by those users over the network.

---

# Overte Headless-Server (Unofficial) (日本語)

## コンセプト：情報は物理に縛られるべきか？

皆さんはVRを楽しんでいますか？
距離を越えて、東京、大阪、あるいは海外にいる人とも同じ空間を共有できる。本当に素晴らしい技術です。中でもOverteの最大の強みは、「特定の大企業が管理するプラットフォーム」ではなく、「あなた自身が管理・所有できる空間」を持てることでしょう。

しかし、ここで一つ問いかけさせてください。

**その「あなたの部屋」は、いま何に依存して生きていますか？**

誰かのパソコンでホストしていますか？
もしそのマシンの電源が落ちてしまったら、その空間にはもうアクセスできないのでしょうか？
あなたのVR空間の命は、特定のハードウェアという「物理」に縛り付けられてしまっていませんか？

本来、情報とは特定の物理媒体に縛られるべきものではなく、もっと流動的であるべきです。
もし、「特定のマシンに依存しない部屋」を作ることができたなら。あるサーバーが使えなくなっても、別の誰かのパソコンでさっと立ち上げ直すだけで空間が蘇るなら、そのVR空間は生き続けていると言えます。それはまるで人間の記憶のように、誰か1人が引き継いでさえいれば、その情報（空間）は死なないのです。

そこで私は、Overteのポータブルなヘッドレス・サーバー（HS）化を構想しました。

このプロジェクトが目指すのは、インストール作業をはじめとした環境依存からの脱却です。
- **単一の実行ファイル**で構成され、インストールは不要。
- **管理者権限不要**で、ポート番号を指定するだけで即座にlocalhostでVR空間をホスト。
- **完全なポータビリティ**: 空間に関する全データは、起動時のカレントディレクトリ配下の `data/` フォルダ内に保存（`--data <dir>` で任意の場所に変更可能）。

つまり、この `data/` フォルダさえ別のマシンにコピーすれば、OSやCPUの違いを一切問わず、全く同じ部屋をどこでもホストできるようにしたいということです。

## 使い方（基本）
使い方の基本としては、以下のように各機能のポートを指定して呼び出します。

```bash
overte-hs --domain 40102 --audio 40103 --avatar 40104 --entity 40105 --assets 40106 --entity-script 40107 --messages 40108
```
これで指定した各ポート（domain: 40102, audio: 40103...）に役割が割り当てられ、VR空間が鼓動し始めます。

> **ポートは自由に選べます。** `--domain` を40102以外（例: `--domain 50102`）にしても、そのdomainへ登録する他のサーバー（audio 50103...）のポートを続けて指定するだけで動きます。ポート番号は設定ファイルに保存されないため、マシンごと・空間ごとに自由に決められます。

### クライアント（Interface）側の接続
Overteクライアントは特定のマシンと特定の部屋が紐づくことを前提として作られているため、特定のマシンで複数の部屋をホストすることを想定しておらず、ポート番号は40102または環境変数で固定されており、GUI内ではポート番号を変更することが出来ません。そのため、Overte HSでホストした部屋に接続するにはクライアントにひと工夫が必要です。
具体的には、サーバーのdomainポートと同じ値を `HIFI_DOMAIN_SERVER_PORT` 環境変数に一時指定してクライアントを起動させる必要があります。
```bash
# サーバーを 50102 で起動した場合、クライアントも同じポートを指定
HIFI_DOMAIN_SERVER_PORT=50102 ./Overte.AppImage
```
```bat
set HIFI_DOMAIN_SERVER_PORT=50102 & "C:\Prpgram Files\Overte\interface.exe"
```
起動後、Interface内で `Ctrl+L` を押し、任意のホスト名を入力して接続します（ポートは環境変数から自動的に採用されます）。

ちなみに、環境変数の設定が無い場合、Overteクライアントはデフォルトポートとして `40102` をハードコードしています。`--domain` を40102以外にした場合は、必ずクライアント側でも `HIFI_DOMAIN_SERVER_PORT` を一致させてください。これを怠ると、クライアントは40102宛にチェックインを送り続け、接続が確立されません。
（ちなみに、アケネＪがローカルサーバを建てて、クライアントで `Ctrl+L`を押して`hifi://localhost:50102`とポートを指定して入室したところ、入室できて2秒くらい経過して切断され、また2秒位経過したら入室できる繰り返しになりました。）

### 空間の分離と分散
生成されたフォルダ（デフォルトで `data/`）内では、サーバーごとの役割が明確に分割されています。

| サーバー機能 | Dataフォルダ内の保存先 |
| --- | --- |
| domain | `config.json`（設定のみ） |
| entity | `entities/`（空間データ） |
| asset | `assets/` |

各サーバーは自身のデータを自律的に管理します。domain-serverはentity-serverのデータを一切保持せず、ネットワーク経由でのみ連携します。これにより、各サーバーを独立にコピー・移動・復元できるようになっています。

例えば、特定のデータフォルダ（ここでは`assets/`）だけを別のマシンにコピーし、以下のようにコマンドを打ってみてください:
```
overte-hs --host 192.168.1.5:40102 --assets 40106
```

これにより、「大元のDomainサーバーは `192.168.1.5:40102` に存在しているが、その部屋のAssets機能だけは今このマシンで処理（ホスト）する」といった分散構成が可能になります。

## 環境依存の排除
上流のOverteでは、「1つのマシンに1つの部屋」が原則になっているらしく、サーバを起動するとマシンの環境変数に`HIFI_DOMAIN_SERVER_PORT`を書き込みますが、Overte HSでは環境変数への書き込みは行いません。理由は、同一マシンの異なるポートで別の部屋をホストする場合に干渉してしまうからです。ちなみに、複数の空間を同一のマシンでホストする時は以下のようにします:
```bash
# 空間A: ポート 40102系
overte-hs --data ./roomA --domain 40102 --audio 40103 --avatar 40104 --entity 40105 --assets 40106 --entity-script 40107 --messages 40108
# 空間B: ポート 50102系（クライアントは HIFI_DOMAIN_SERVER_PORT=50102 で別窓起動）
overte-hs --data ./roomB --domain 50102 --audio 50103 --avatar 50104 --entity 50105 --assets 50106 --entity-script 50107 --messages 50108
```
空間Bへ接続するクライアントだけが `HIFI_DOMAIN_SERVER_PORT=50102` を付けて起動すればよいため、両空間は互いに干渉しません。

### インターネットを介した部屋への接続
上流が実装している中央集権的なサーバへの登録機能はOverte HSでは含めていませんので、Overte HSが行う通信はそのマシンま待ち受けているポートにアクセスしたクライアントとの送受信のみです。インターネットを介して通信をしたい場合はグローバルIPやポート開放などの、Overte HSがサポートしない部分の準備が必要です。
ちなみに、アケネＪはTailscaledのクライアントとして制作したtunnneji-tailを使用しています。tunnneji-tailを用いると特定の人だけに特定のポートをパスワード付きで共有できるので、特定の人しか入れない部屋を作れます。共有相手は40102にnatすれば、一時環境変数の設定が不要になります。

## ライセンス

本プロジェクトの上流からの変更部分は **GNU Affero General Public License v3.0 (AGPLv3)** の下でライセンスされています。

* **上流コード**: 本プロジェクトは[Overte](https://github.com/overte-org/overte) のコードを基にしています。上流コードは引き続き Apache License 2.0 の下で提供されます（`LICENSE`）。
* **変更部分**: ヘッドレス化・分散化などのフォークによる変更部分の著作権はアケネＪに帰属し、AGPLv3 の下で提供されます（`LICENSE-AGPL-3.0.txt`）。詳細は `NOTICE` を参照してください。

AGPLv3 第13条に基づき、本ソフトウェア（またはその改変版）をネットワーク経由でユーザーに提供・利用させる場合、改変を含む完全な対応ソースコード（Corresponding Source）を、ネットワーク経由でそのユーザーが無償で取得できるようにする義務が生じます。
