**Credit CHANGELOG**
-------------------------

**Credit v1.0.2.0**

* [BDAP] Set maximum months stored in local accounts db
* [BDAP] Limit acount registration to 100 years
* [BDAP] Remove maximum months for updated accounts
* [BDAP] Fix add months to block and epoch times
* [Mempool] Don't check if standard tx for BDAP txs in accept to mempool
* [Util] Fix epoch to ISO date string conversion
* [Wallet] Check txout instead of entire tx for BDAP
* Add Mainnet checkpoints
* Bump client and block version to v1.0.2.0
* Change SporkAddress
* Increase protocol to 71100 or greater
* Increase minimum protocol to 71000 or greater


**Credit v1.0.1.0**

* [Qt] Update/Add Languages
* [BDAP] Fix send BDAP transaction after removing credits from wallet GetBalance() method
* [Wallet] Prevent transactions that require Ed25519 keys if wallet needs upgrading. Notify user.
* [Wallet] Prevent ed25519 keypool crash when trying to erase from an empty set
* [FIX] Fix locked wallet upgrade issues.
* [RPC] Refactor all RPC code files in a seperate directory
* [LOG] Silence LogPrint when not finding ServiceNodes with votes
* [DHT] Return an empty JSON array when the denylink record is not found
* [HD] Move mnemonic wordlists to separate directory
* [FIX] Correct versioning for alert max version
* [Wallet] Remove BDAP credits from GetBalance method
* [BDAP] Fix send BDAP transaction after removing credits from wallet GetBalance() method


**Credit v1.0.0.0**

* Increase ServiceCollateral to 50,000 0AC

** Initial Fork from Duality
