# Phase 4 — FIX 4.2 / 4.4 / 5.0SP2 Dictionary Tables (authoritative reference)

Reference used to rebuild the tables in `src/dictionary/data_dictionary.cpp`. Compact, table-first; every claim carries its source.

**Markers.** `✔` = ≥2 independent sources agree (value/flag identical). `⚠` = sources disagree; resolution stated in the row. `UNVERIFIED` = no second source — do not ship without checking the official spec PDF. **strict** = must always be present; **cond** = required only when its component/group is present.

**Standing policy (apply to the whole engine):** *accept the superset, send the strict subset.* Receiving a deprecated-but-dictionary-listed value must not produce `35=j`; emitting one must never happen.

## 0. Sources

| # | Source | URL | Used for |
|---|---|---|---|
| S1 | QuickFIX (C++) spec XML | `https://raw.githubusercontent.com/quickfix/quickfix/master/spec/{FIX42,FIX44,FIX50SP2,FIXT11}.xml` | enums, required sets, group members |
| S2 | QuickFIX/J spec XML | `https://raw.githubusercontent.com/quickfix-j/quickfixj/master/quickfixj-messages/quickfixj-messages-{fix42,fix44,fix50sp2}/src/main/resources/{FIX42,FIX44,FIX50SP2}.xml` | independent cross-check of S1 |
| S3 | OnixS dictionary (tag pages) | `https://www.onixs.biz/fix-dictionary/4.2/tagNum_<n>.html`, `.../4.4/...`, `.../5.0.sp2.ep302/...`, `.../FIXT1.1/...` | enums + `"Req'd"` flags, deprecated markers |
| S4 | OnixS message pages | `https://www.onixs.biz/fix-dictionary/4.2/msgType_<t>_<id>.html` (same for `4.4`, `5.0.sp2.ep302`, `FIXT1.1`) | per-message required lists and group expansion |
| S5 | FIX Trading Community Orchestra | `https://raw.githubusercontent.com/FIXTradingCommunity/orchestrations/master/FIX%20Standard/{OrchestraFIX42,OrchestraFIX44,OrchestraFIXLatest,FIXTSession}.xml` | official presence flags incl. components (namespace `fixprotocol.io/2020/orchestra`) |
| S6 | OnixS FIXT 1.1 session-protocol page | `https://www.onixs.biz/fix-dictionary/fixt1.1/section_session_protocol.html` | ApplVerID precedence rules |

Notes: S3 plain `5.0.sp2` pages are an *older Extension Pack snapshot* and disagree with S1/S2 on several enums — **use `5.0.sp2.ep302`** for FIX 5.0SP2 (EP302 == S1 for all of §1). `OrchestraFIXLatest.xml` = "FIX Latest as of EP312", not byte-identical to 5.0SP2. S2 is `quickfix-j/quickfixj` (not `quickfixengine/…`).

---

## 1. Version-dependent enumerations

### 1.1 Master enum table (values as accepted on receive)

| Tag | Name | FIX 4.2 | FIX 4.4 | FIX 5.0SP2 |
|---|---|---|---|---|
| 40 | OrdType | `1-9,A-I,P` | `1,2,3,4,6,7,8,9,D,E,G,I,J,K,L,M,P` † | `1-9,A-M,P,Q,R,S` |
| 59 | TimeInForce | `0-6` | `0-7` | `0-9,A,B,C` |
| 150 | ExecType | `0-9,A-E` | `0,3-9,A-I` ‡ | `0,3-9,A-N` |
| 39 | OrdStatus | `0-9,A-E` | `0-4,6-9,A-E` § | `0-9,A-E` |
| 54 | Side | `1-9` | `1-9,A-G` | `1-9,A-H` |
| 434 | CxlRejResponseTo | `1,2` | `1,2` | `1,2` |
| 269 | MDEntryType | `0-9` | `0-C` | `0-9,A-H,J-Z,a-i,t` |
| 279 | MDUpdateAction | `0,1,2` | `0,1,2` | `0-5` |
| 263 | SubscriptionRequestType | `0,1,2` | `0,1,2` | `0,1,2` |
| 380 | BusinessRejectReason | `0-5` | `0-7` | `0-7,8,9,10,18` |
| 103 | OrdRejReason | `0-8` | `0-15,99` ¶ | `0-16,18-33,99` |
| 102 | CxlRejReason | `0-3` | `0-6,99` | `0-8,18,99` |
| 373 | SessionRejectReason | `0-11` | `0-17,99` | `0-18,99` (FIXT.1.1 same) |
| 281 | MDReqRejReason | `0-8` | `0-9,A-C` | `0-9,A-C` |
| 98 | EncryptMethod | `0-6` | `0-6` | `0-6` |
| 1128 | ApplVerID | n/a | n/a | `0-9` + `10`(FIXLatest) ⚠ |
| 43/97/123/141 | PossDupFlag/PossResend/GapFillFlag/ResetSeqNumFlag | `Y\|N` | `Y\|N` | `Y\|N` |

All rows `✔` (S1=S2=S3/S4=S5) unless footnoted.

† S1/S2 `FIX44.xml` drop `5,A,B,C,F,H`; S3 lists them as *"No longer used"*. **Resolution:** accept `5,A,B,C,F,H` on receive (no `35=j`), never send. ‡ S3/S2 still list `1,2` as *"(Replaced)"*; S1 drops them. **Resolution:** accept, never send. § `5`=Replaced is *deprecated in FIX.4.3* and absent from S1 `FIX44.xml` but present in S3/S2 and back in 5.0SP2 → **accept in 4.4 too**. ¶ S1 `FIX44.xml` omits `12` (Invalid/Deprecated delivery); S2 and S3 include it → **include 12**.

⚠ `1128 ApplVerID`: S1 `FIXT11.xml` + S2 + S3 `FIXT1.1` = `0-9`; S1 `FIX50SP2.xml` + S5 `FIXTSession.xml` add `10=FIXLatest`. **Resolution:** accept `0-10`. No source found for `99` → **UNVERIFIED, do not accept** unless a counterparty profile requires it.

### 1.2 Selected value meanings (S1 descriptions)

| Tag | FIX 4.4 | FIX 5.0SP2 additions/changes |
|---|---|---|
| 40 OrdType | `1`MARKET `2`LIMIT `3`STOP `4`STOP_LIMIT `6`WITH_OR_WITHOUT `7`LIMIT_OR_BETTER `8`LIMIT_WITH_OR_WITHOUT `9`ON_BASIS `D`PREVIOUSLY_QUOTED `E`PREVIOUSLY_INDICATED `G`FOREX_SWAP `I`FUNARI `J`MARKET_IF_TOUCHED `K`MARKET_WITH_LEFT_OVER_AS_LIMIT `L`PREV_FUND_VALUATION_PT `M`NEXT_FUND_VALUATION_PT `P`PEGGED | + `5`MARKET_ON_CLOSE `A`ON_CLOSE `B`LIMIT_ON_CLOSE `C`FOREX_MARKET `F`FOREX_LIMIT `H`FOREX_PREV_QUOTED `Q`COUNTER_ORDER_SELECTION `R`STOP_ON_BID_OR_OFFER `S`STOP_LIMIT_ON_BID_OR_OFFER |
| 59 TimeInForce | `0`DAY `1`GTC `2`AT_THE_OPENING `3`IOC `4`FOK `5`GTX `6`GTD `7`AT_THE_CLOSE | + `8`GOOD_THROUGH_CROSSING `9`AT_CROSSING `A`GOOD_FOR_TIME `B`GOOD_FOR_AUCTION `C`GOOD_FOR_MONTH |
| 150 ExecType | `0`NEW `3`DONE_FOR_DAY `4`CANCELED `5`REPLACED `6`PENDING_CANCEL `7`STOPPED `8`REJECTED `9`SUSPENDED `A`PENDING_NEW `B`CALCULATED `C`EXPIRED `D`RESTATED `E`PENDING_REPLACE `F`TRADE `G`TRADE_CORRECT `H`TRADE_CANCEL `I`ORDER_STATUS | + `J`TRADE_IN_A_CLEARING_HOLD `K`TRADE_RELEASED_TO_CLEARING `L`TRIGGERED_OR_ACTIVATED `M`LOCKED `N`RELEASED |
| 39 OrdStatus | `0`NEW `1`PARTIALLY_FILLED `2`FILLED `3`DONE_FOR_DAY `4`CANCELED `6`PENDING_CANCEL `7`STOPPED `8`REJECTED `9`SUSPENDED `A`PENDING_NEW `B`CALCULATED `C`EXPIRED `D`ACCEPTED_FOR_BIDDING `E`PENDING_REPLACE | + `5`REPLACED |
| 54 Side | `1`BUY `2`SELL `3`BUY_MINUS `4`SELL_PLUS `5`SELL_SHORT `6`SELL_SHORT_EXEMPT `7`UNDISCLOSED `8`CROSS `9`CROSS_SHORT `A`CROSS_SHORT_EXEMPT `B`AS_DEFINED `C`OPPOSITE `D`SUBSCRIBE `E`REDEEM `F`LEND `G`BORROW | + `H`SELL_UNDISCLOSED |

### 1.3 Source conflicts inside §1 (all resolved above)

| Field | S1 (QF) | S2 (QFJ) | S3 (OnixS) | Resolution |
|---|---|---|---|---|
| 40 @4.4 | drops `5,A,B,C,F,H` | includes (deprecated) | includes | accept superset |
| 150 @4.4 | drops `1,2` | includes | includes | accept superset |
| 39 @4.4 | drops `5` | includes | includes | accept superset |
| 103 @4.4 | drops `12` | includes `12` | includes `12` | include `12` |
| 40/59/150/54/269/380/103 @5.0SP2 | =EP302 | older (fewer) | `5.0.sp2.ep302` = S1; plain `5.0.sp2` older | use EP302 values |
| 103 @5.0SP2 | lacks `30,31,32,33` | lacks them | EP302 has them | include `30-33` |
| 269 @5.0SP2 | lacks `f` | lacks `f` | EP302 has `f` | include `f` |
| 1128 | `0-9` (FIXT11) / `0-10` (50SP2) | `0-9` | `0-9` | accept `0-10` |

**Section sources:** S1, S2, S3, S4, S5.

---

## 2. Required / optional fields per message

Strictly-required **body** tags (header/trailer excluded). `✔` = S1=S2=S4=S5 agree.

### 2.1 Application messages

| MsgType | Name | FIX 4.2 | FIX 4.4 | FIX 5.0SP2 |
|---|---|---|---|---|
| `D` | NewOrderSingle | `11,21,55,54,60,40` ✔ | `11,54,60,40` ✔ | `11,54,60,40` ✔ |
| `8` | ExecutionReport | `37,17,20,150,39,55,54,151,14,6` ✔ | `37,17,150,39,54,151,14,6` ✔ | `37,17,150,39,54,151,14` ✔ |
| `F` | OrderCancelRequest | `41,11,55,54,60` ✔ | `41,11,54,60` ✔ | `11,54,60` ✔ ⚠(no `41`) |
| `G` | OrderCancelReplaceRequest | `41,11,21,55,54,60,40` ✔ | `41,11,54,60,40` ✔ | `11,54,60,40` ✔ ⚠(no `41`) |
| `9` | OrderCancelReject | `37,11,41,39,434` ✔ | `37,11,41,39,434` ✔ | `37,11,39,434` ✔ |
| `V` | MarketDataRequest | `262,263,264,+267,+146` ✔ | `262,263,264,+267,+146` ✔ | `262,263,264,+267,+146` ⚠ |
| `W` | MD Snapshot/FullRefresh | `55,+268` ✔ | `+268` ✔ | `+268` ⚠ (≠`779`) |
| `X` | MD IncrementalRefresh | `+268` ✔ | `+268` ✔ | `+268` ⚠ |
| `j` | BusinessMessageReject | `372,380` ✔ | `372,380` ✔ | `372,380` ✔ |

`+NNN` = the group count tag itself is strict; its members are in §2.2.

⚠ **5.0SP2 `V`/`W`/`X`:** S3-EP302 marks components `<MDReqGrp>`, `<InstrmtMDReqGrp>`, `<MDFullGrp>`, `<MDIncGrp>` **required**, S2 marks the groups inside them required, S5 marks them required; S1 marks the inner count tags `required=N` — systematically, in **all 22 group-bearing components** of `FIX50SP2.xml` (S2 has them `Y` in the same 22). **Resolution: require the group occurrence** (3 sources) — config flag `strict_md_groups=false` only if a QuickFIX peer trips it. ⚠ **5.0SP2 `W`:** S1 alone requires `779 LastUpdateTime`; S3-EP302 says `N`, S2 (older EP) has no such field, S5 has it **optional** → **do not require `779`**. ⚠ **5.0SP2 `F`/`G` `41 OrigClOrdID`:** S1=S2=S3-EP302 all say `N` ("required when referring to orders electronically submitted…") → **not strictly required** (4.4 still requires it).

### 2.2 Strict members *inside* required groups

| Version | Group | Strict members per repetition |
|---|---|---|
| 4.2 | 267 NoMDEntryTypes | `269` |
| 4.2 | 146 NoRelatedSym | `55` (then 65,48,22,167,200,205,201,202,206,231,223,207,106,348,349,107,350,351 opt) |
| 4.4 / 5.0SP2 | 146 NoRelatedSym | none strict — `<Instrument>` component is required but every member is optional (S1 `Symbol=N`, S4 shows no `Symbol` row, S5 `opt`) ⇒ **do not require `55`**; ⚠ S2's FIX44 `Instrument.Symbol=Y` is the outlier (§2.5) |
| 4.2 | 268 in `W` | `269`, `270` |
| 4.4 / 5.0SP2 | 268 in `W` | `269` only (`270` optional) |
| all | 268 in `X` | `279` only (`269` optional) |
| all | 267 | `269` |

### 2.3 Session messages (S1 `FIXT11.xml` = S4 `FIXT1.1` = S5 `FIXTSession.xml`)

| MsgType | Name | Required (body) |
|---|---|---|
| `0` | Heartbeat | — |
| `1` | TestRequest | `112` |
| `2` | ResendRequest | `7,16` |
| `3` | Reject | `45` (optional: `371,372,373,58,354,355`) |
| `4` | SequenceReset | `36` |
| `5` | Logout | — (`58` optional) |
| `A` | Logon (FIXT.1.1) | `98,108,1137` ✔ |
| `A` | Logon (FIX 4.2 / 4.4) | `98,108` ✔ (no `1137` exists pre-FIXT) |

### 2.4 Standard header / trailer (all versions)

**Strict:** `8 BeginString, 9 BodyLength, 35 MsgType, 49 SenderCompID, 56 TargetCompID, 34 MsgSeqNum, 52 SendingTime` and trailer `10 CheckSum`. **Optional:** `43,50,57,90/91,93/95/96,115,116,122,128,129,142-145,347,354,355,369,370,627 NoHops` (4.4+), `1128/1129/1156` (FIXT.1.1 header). `BeginString` must be `FIX.4.2`, `FIX.4.4` or `FIXT.1.1` (5.0SP2 runs over FIXT.1.1).

### 2.5 Component rule (why some "required" components impose no tag)

S3/S5 mark `<Instrument>`, `<OrderQtyData>`, `<StandardHeader>` (etc.) required, but a component with **no required member** requires *no individual tag*. Correct strict sets above already reflect that (that is why 4.4/5.0SP2 `D` is only `11,54,60,40`). Never translate "component required" into "require `55`/`38`".

Cross-source component conflicts found while building §2 (resolve with the majority, i.e. the rows above):

| Version | Component member | S1 QF | S2 QFJ | S3/S4/S5 | Resolution |
|---|---|---|---|---|---|
| 4.4 | `Instrument.Symbol` | `N` | **`Y`** | optional (S4 no row, S5 `opt`) | optional — expanding S2 literally would require `55` in `D,F,G,8,V` and contradict 3 sources |
| 4.4 | `UnderlyingInstrument.UnderlyingSymbol` | `N` | **`Y`** | optional | optional (same reasoning) |
| 4.4 | inner counts of `PositionAmountData`, `PositionQty`, `TrdRegTimestamps` | `N` | `Y` | — | prefer `N` (those groups are optional anyway) |
| 5.0SP2 | inner count tag of **22** group components (`MDReqGrp`, `MDFullGrp`, `MDIncGrp`, `InstrmtMDReqGrp`, `LegOrdGrp`, `LinesOfTextGrp`, …) | `N` | `Y` | required (S3-EP302 component `Y`, S5 `Y`) | **require** — see §2.1 |

**Section sources:** S1, S2, S4, S5 (S4 for OnixS structure tables incl. `=>` group nesting).

---

## 3. Repeating groups

### 3.1 Inventory for the core messages

| Msg | Count tag | Structure name | Strict at msg level? | 4.2 | 4.4 | 5.0SP2 |
|---|---|---|---|---|---|---|
| `D`,`G` (+`8` @5.0SP2) | 78 | NoAllocs (`PreAllocGrp` 4.4+) | no | ✔ | ✔ | ✔ ⚠ |
| `D`,`F`,`G`,`8` (4.4+) ; +`V` @5.0SP2 | 453 | NoPartyIDs (`Parties` component) | no | — (no Parties in 4.2) | ✔ | ✔ |
| `D`,`G` (all) ; `V` (4.4+) | 386 | NoTradingSessions (`TrdgSesGrp`) | no | ✔ | ✔ | ✔ — **not** in `F` or `8` (those carry flat `336`/`625`) |
| `8` | 382 | NoContraBrokers (`ContraGrp` 4.4+) | no | ✔ | ✔ | ✔ |
| header | 627 | NoHops | no | — | ✔ | ✔ |
| `V` | 267 | NoMDEntryTypes (`MDReqGrp`) | **yes** | ✔ | ✔ | ✔ ⚠ |
| `V` | 146 | NoRelatedSym (`InstrmtMDReqGrp`) | **yes** | ✔ | ✔ | ✔ ⚠ |
| `W` | 268 | MDFullGrp (`NoMDEntries`) | **yes** | ✔ | ✔ | ✔ ⚠ |
| `X` | 268 | MDIncGrp (`NoMDEntries`) | **yes** | ✔ | ✔ | ✔ ⚠ |
| `W` (555) ; `D,F,G,W` (711) | 555 / 711 | NoLegs / NoUnderlyings | no | — | ✔ | ✔ |

⚠ = the 5.0SP2 group-presence conflict of §2.1.

### 3.2 Member lists (`*` = strict inside the group; `-` = group absent)

| Group (count) | 4.2 | 4.4 | 5.0SP2 |
|---|---|---|---|
| 267 NoMDEntryTypes | `269*` | `269*` | `269*` |
| 146 NoRelatedSym | `55*`,65,48,22,167,200,205,201,202,206,231,223,207,106,348,349,107,350,351,336 (20) | `<Instrument>`(all opt) + `711`,`555` (3) — **no strict member** | as 4.4 + EP fields ⚠ |
| 268 in `W` | `269*`,`270*` + 15,271,272,273,274,275,336,276,277,282,283,284,286,59,432,126,110,18,287,37,299,288,289,346,290,58,354,355 (30) | `269*` + 270,15,271,272,273,274,275,336,**625**,276,277,282,283,284,286,59,432,126,110,18,287,37,299,288,289,346,290,**546,811**,58,354,355 (33) | `269*` + as 4.4 with EP-added fields ⚠ |
| 268 in `X` | `279*` + 285,269,278,280,55,65,48,22,167,200,205,201,202,206,231,223,207,106,348,349,107,350,351,291,292,270,15,271,272,273,274,275,336,276,277,282,283,284,286,59,432,126,110,18,287,37,299,288,289,346,290,387,58,354,355 (56) | `279*` + 285,269,278,280,`<Instrument>`,711,555,291,292,270,15,271,272,273,274,275,336,625,276,277,282,283,284,286,59,432,126,110,18,287,37,299,288,289,346,290,546,811,451,58,354,355 (43) | `279*` + as 4.4 with EP-added fields ⚠ |
| 78 NoAllocs (in `D`/`G`) | `79 AllocAccount`,`80 AllocShares` (both opt) | `79 AllocAccount,661 AllocAcctIDSource,736 AllocSettlCurrency,467 IndividualAllocID,<NestedParties 539>,80 AllocQty` (all opt) | as 4.4 + EP fields ⚠ |
| 453 NoPartyIDs | — | `448,447,452` + nested `802 NoPartySubIDs`(`523,803`) | as 4.4 (+`2376` EP) ⚠ |
| 539 NoNestedPartyIDs | — (`539` does not exist in 4.2) | `524 NestedPartyID,525 NestedPartyIDSource,538 NestedPartyRole` + nested `804 NoNestedPartySubIDs`(`545,805`) | as 4.4 |
| 386 NoTradingSessions | `336` only | `336,625` | S1=`336,625`; S2 (older EP) lists ~22 members → ⚠ EP-dependent |
| 382 NoContraBrokers | `375,337,437,438` | + `655` | + `655` |
| 627 NoHops | — | `628,629,630` | `628,629,630` |
| 711/555 | — | component-based (`<UnderlyingInstrument>`, `<InstrumentLeg>`) — flat lists differ between sources → **UNVERIFIED: load from pinned XML** | same |

⚠ 5.0SP2 group members differ between S1 (newer EP) and S2 (older EP) wherever EPs added fields (78, 146, 268, 386, 453, 539, 555). For 5.0SP2 take the **union** of S1 and S3-EP302 for `4.4`-era fields and mark EP-added members `UNVERIFIED` until the target EP is pinned.

### 3.3 Same count tag, different members (never dedupe by tag)

| Count | Context A | Context B |
|---|---|---|
| 268 | in `W`: `269*`,`270*`(4.2) | in `X`: `279*`, `269` optional |
| 78 | in `D`/`G` (PreAllocGrp): `79,661,736,467,539,80` | in AllocationInstruction (AllocGrp): ~28 members incl. 366,776,161,360,361 |
| 73 | OrderMassCancelReport variant | ListStatus variant (74 vs 5 members in S1/S2) |

⇒ `MessageDef` must carry **per-message** group definitions, not a global tag→members map.

### 3.4 Group validation rules

1. The count tag must be present (when the group is strict) and its integer value must equal the number of repetitions — mismatch → `373=16` (4.4+/FIXT) or `373=5` (4.2, `16` did not exist).
2. Each repetition is contiguous: a field belonging to the group may not appear after a field that ends the group → `373=15` (4.4+/FIXT); in 4.2 use `373=5`.
3. The count tag itself is the first field of every repetition; nested groups carry their own count tag (`453→802`, `539→804`, `948→952`).
4. A field may not appear twice inside one repetition → `373=13` (4.4+/FIXT), `373=5` (4.2).
5. Optional groups: absent count tag is valid; count `0` is valid but pointless — accept it, do not reject.

**Section sources:** S1, S2, S4, S5.

---

## 4. Validation rules

### 4.1 Check → `373 SessionRejectReason` by version

| Check | FIX 4.2 | FIX 4.4 | FIX 5.0SP2 / FIXT.1.1 |
|---|---|---|---|
| Tag not in dictionary (undefined) | `3` | `3` | `3` |
| Tag defined but not used by this MsgType | `2` | `2` | `2` |
| Strict tag missing | `1` | `1` | `1` |
| Enum value not in §1 set / value out of range | `5` | `5` | `5` |
| Wrong data type or format (e.g. `52` ≠ `YYYYMMDD-HH:MM:SS.s{0,9}`) | `6` | `6` | `6` |
| Tag specified with no value (`11=`) | `4` | `4` | `4` |
| Tag appears more than once | *use `5`* ⚠ | `13` | `13` |
| Tags out of dictionary order | *use `5`* ⚠ | `14` | `14` |
| Repeating-group fields out of order | *use `5`* ⚠ | `15` | `15` |
| Incorrect `NumInGroup` count | `5` | `16` | `16` |
| Non-data value contains `<SOH>` | *use `6`* ⚠ | `17` | `17` |
| Invalid/unsupported `ApplVerID(1128)` | n/a | n/a (pre-FIXT) | `18` |
| Invalid session `MsgType(35)` | `11` | `11` | `11` |
| Unknown *application* MsgType | send `35=j` with `380=2` (Unsupported message type) + `58`; **do not** use `373` — `j` is the app-layer vehicle ⚠ UNVERIFIED mapping, confirm against target counterparty spec |

⚠ 4.2 has only `0-11` in `373` (S1=S2=S3), so reasons `13-17` must be collapsed onto `5`/`6` for FIX 4.2 sessions.

### 4.2 Session-layer rules (not dictionary checks)

* Framing: `8` first, `9` second (byte length of body), `35` third, `10` last; recompute and compare `9`/`10` → on mismatch, disconnect (not `35=3`).
* Sequence: expected `34`; gap → `ResendRequest(2)`; duplicate with `43=Y` → discard silently; `SequenceReset(4)` with `123=Y` → gap fill, set `36`.
* Heartbeat: `108` required on Logon; `0` invalid (logout). TestReqID echo required on `1`.
* `35=3` Reject is reserved for protocol violations; dictionary/semantic failures of application messages are business rejections (`35=j`, `380=0` + `58`) — matches current `session.cpp` behaviour.

### 4.3 Choosing the dictionary version (S6)

Precedence: **explicit `ApplVerID(1128)` > message-type default (`RefMsgType(372)` + `RefApplVerID(1130)` + `DefaultVerIndicator(1410)`) > session default `DefaultApplVerID(1137)`.** `1128`/`1156`/`1129` are **not permitted on session-level messages**. Validate application messages against the *resolved* dictionary, not against `BeginString` (which is `FIXT.1.1` for 5.0SP2).

### 4.4 Engine policy recommendations

| # | Rule | Confidence |
|---|---|---|
| P1 | Accept superset, send strict subset (§0) | High |
| P2 | Resolve version via `1128`/`1137`; unknown → reject `373=18` (FIXT) | High |
| P3 | Required-tag failure → `35=j`, `380=0`, `58` naming the missing tag; enum failure → `380=0` + tag/value (there is no `380` for "bad enum") | High |
| P4 | `NumInGroup` mismatch → `373=16` (4.4+/5.0), `373=5` (4.2) | High |
| P5 | Never validate a MsgType the dictionary does not define — but *log* it (current code silently skips; see §5.3) | High |
| P6 | Deprecated values (§1.2) accepted on receive only | High |

**Section sources:** S1 (`FIXT11.xml`), S3 (`FIXT1.1` tag pages + `section_session_protocol.html`), S5 (`FIXTSession.xml`), S6.

---

## 5. Delta vs `src/dictionary/data_dictionary.cpp`

Current state: one shared `load_builtin_fields()` (L84) for all versions, `load_builtin_messages_44()` (L229) just calls `_42()`, `load_builtin_messages_50sp2()` (L235) calls `_44()`; `validate()` (L337) performs exactly two checks — required-tag presence (L346-350) and enum values (L353-359) — and **skips tags that have no `FieldDef`** (L355-356), so unknown/undefined tags are accepted. Failures surface as `35=j`, `380=0` + `58` via `Session::send_business_reject` in `session.cpp:594-614` (message never dispatched); a MsgType the dictionary does not define is skipped entirely (L598 `find_message`).

### 5.1 Enum deltas (all versions share one table → every row is version-wrong)

| Tag | Current table | Correct 4.2 | Correct 4.4 | Correct 5.0SP2 | Impact |
|---|---|---|---|---|---|
| 40 OrdType | `1-9,A-D` | needs `E,F,G,H,I,P` | needs `E,G,I,J,K,L,M,P` (drop `5,A,B,C,F` for send) | needs `E-M,P-S` | **FR** (e.g. 4.4/5.0SP2 `J`=StopLimit) / FA |
| 39 OrdStatus | `0-9,A-C` | needs `D,E` | needs `D,E` | needs `D,E` | **FR** on `E`=PendingReplace |
| 150 ExecType | `0-9,A-F` | OK (+drop `F`) | needs `G,H,I` (drop `1,2`) | needs `G-N` (drop `1,2`) | **FR** TradeCorrect/TradeCancel/OrderStatus |
| 54 Side | `1-9` | OK | needs `A-G` | needs `A-H` | **FR** on A-G/H |
| 59 TimeInForce | `0-9` | drop `7,8,9` (FA only) | needs `7`; drop `8,9` | needs `8,9,A,B,C` | **FR** in 5.0SP2 |
| 269 MDEntryType | `0-9,A-J` | drop `A-J` (FA) | drop `D-J` (FA) | needs `K-Z,a-i,t` | **FR** in 5.0SP2 / FA in 4.2/4.4 |
| 279 MDUpdateAction | `0,1,2` | OK | OK | needs `3,4,5` | **FR** in 5.0SP2 |
| 434 CxlRejResponseTo | `1,2,3` | drop `3` | drop `3` | drop `3` | FA (accepts bogus `3`) |
| 1128 ApplVerID | `0-9` (50sp2 only) | n/a | n/a | needs `10` | **FR** of `10`=FIXLatest |
| 103/102/380/281/373 | *no enum at all* | define §1 sets | define §1 sets | define §1 sets | no validation → FA; outbound values unchecked |
| 263, 98, booleans | `0,1,2` / `0-6` / `Y,N` | ✔ | ✔ | ✔ | none |

*FR = false reject (valid inbound message → `35=j`), FA = false accept.*

### 5.2 Message required-set deltas

Current required lists (shared, 4.2-based) vs spec. `+` = we require it but the spec does not (FR); `−` = spec requires it and we do not (FA).

| Msg | Version | Current required | Spec | Delta |
|---|---|---|---|---|
| `D` | 4.2 | 11,21,55,54,60,40,**38** | 11,21,55,54,60,40 | **+38 → FR** (CashOrderQty 64 orders) |
| `D` | 4.4 | 11,21,55,54,60,40,38 | 11,54,60,40 | **+21,+55,+38 → FR** |
| `D` | 5.0SP2 | 11,21,55,54,60,40,38 | 11,54,60,40 | **+21,+55,+38 → FR** |
| `8` | 4.2 | 37,17,150,39,55,54,151,14,6 | + `20` ExecTransType | **−20 → FA** |
| `8` | 4.4 | as above | 37,17,150,39,54,151,14,6 | **+55 → FR** |
| `8` | 5.0SP2 | as above | 37,17,150,39,54,151,14 | **+55,+6 → FR** |
| `F` | 4.2 / 4.4 | 41,11,55,54,60 | 4.2: same; 4.4: 41,11,54,60 | 4.4 **+55 → FR** |
| `F` | 5.0SP2 | 41,11,55,54,60 | 11,54,60 | **+41,+55 → FR** |
| `G` | 4.2 | 41,11,21,55,54,60,40,38 | 41,11,21,55,54,60,40 | **+38 → FR** |
| `G` | 4.4 | same | 41,11,54,60,40 | **+21,+55,+38 → FR** |
| `G` | 5.0SP2 | same | 11,54,60,40 | **+41,+21,+55,+38 → FR** |
| `9` | all | *message not defined* | see §2.1 | validation silently skipped (P5) |
| `V` | all | 262,263,264,267,146 | same | ✔ |
| `W` | 4.2 | 55,268 | 55,268 | ✔ |
| `W` | 4.4 / 5.0SP2 | 55,268 | 268 | **+55 → FR** |
| `X` | all | 268 | 268 | ✔ |
| `j` | all | 372,380 | 372,380 | ✔ |
| `A` (FIXT) | 5.0SP2/FIXT | 98,108 | 98,108,**1137** | **−1137 → FA** |

### 5.3 Structural gaps (no `MessageDef` support today)

| Gap | Effect |
|---|---|
| `MsgType 9` OrderCancelReject absent | inbound `9` never validated (skipped by `session.cpp:598`) |
| No group model in `MessageDef` | §3.2/§3.4 unimplementable → no count/order checks |
| No tag-in-MsgType check | `373=2`/`3=…` cannot be produced; foreign tags accepted |
| No duplicate/order/format checks | `13,14,15,16,17` unreachable; format errors not caught |
| `DataDictionary::load()` ignores `path` (L290) | full dictionaries can't be loaded; tables stay hand-written |
| `resolve_appl_ver_id()` (L398) lacks `0-3`, `10` | `DefaultApplVerID=0…3`/`10` → `FixVersion::Unknown` |
| Single field table for 3 versions | §5.1 cannot be fixed without a per-version override layer |
| No `FieldDef` for `380`, `281` and count tags `78,453,382,386,555,711,627` (constants exist in `constants.hpp` only) | harmless today (L355 skips undefined tags) but blocks any `373=3` check and group counting |

### 5.4 Top-10 false-reject bugs (ranked by breadth of valid traffic killed)

| # | Bug | Where | Valid messages rejected |
|---|---|---|---|
| 1 | `D`/`G` require `21 HandlInst` on FIX 4.4 / 5.0SP2 | L205, L214 | every modern order/replace that omits `21` |
| 2 | `40 OrdType` set `1-9,A-D` | L141 | 4.4/5.0SP2 `J,K,L,M,P,Q,R,S`; 4.2 `E-I,P` |
| 3 | `39 OrdStatus` set `0-9,A-C` | L136 | `E` PendingReplace / `D` AcceptedForBidding ERs (all versions) |
| 4 | `54 Side` set `1-9` | L139 | `A-G` (4.4) / `A-H` (5.0SP2) |
| 5 | `150 ExecType` set `0-9,A-F` | L134 | `G,H,I` (4.4) and `J-N` (5.0SP2) |
| 6 | `59 TimeInForce` set `0-9` | L145 | `A,B,C` (5.0SP2) GoodForTime/Auction/Month |
| 7 | `269 MDEntryType` / `279 MDUpdateAction` | L168, L174 | 5.0SP2 MD (`K-Z,a-i,t`; `3,4,5`) |
| 8 | `D`/`G` require `38 OrderQty` (all versions) | L205, L214 | `CashOrderQty(64)` / component-only qty orders |
| 9 | `8` requires `55 Symbol` (4.4) and `55`+`6 AvgPx` (5.0SP2); `W` requires `55` (4.4/5.0SP2) | L209, L220 | fills/quotes identified by `48 SecurityID`, or `AvgPx` omitted |
| 10 | `F`/`G` require `41 OrigClOrdID` on 5.0SP2 (optional there) | L212, L214 | 5.0SP2 cancels/replaces omitting `41` |

Runners-up (false *accepts*, fix while editing): `−20` in 4.2 `8`; `434=3`; missing `103/102/380/281/373` enums; `MsgType 9` unvalidated; no `373=2/3/13-17` checks; `1128` missing `10`.

**Section sources:** S1, S2, S3, S4, S5; current code `src/dictionary/data_dictionary.cpp` (L84, L134-175, L180-248, L337-363), `src/session/session.cpp` (L594-614, L1463) and `src/parser/parser.cpp` (L31-59 header whitelist).

---

## References

1. QuickFIX spec XML (FIX 4.2/4.4/5.0SP2/FIXT1.1) — `https://raw.githubusercontent.com/quickfix/quickfix/master/spec/`
2. QuickFIX/J spec XML — `https://github.com/quickfix-j/quickfixj/blob/master/quickfixj-messages/`
3. OnixS FIX Dictionary, per-tag pages — `https://www.onixs.biz/fix-dictionary/{4.2,4.4,5.0.sp2.ep302,FIXT1.1}/tagNum_{tag}.html`
4. OnixS FIX Dictionary, per-message pages — `https://www.onixs.biz/fix-dictionary/{4.2,4.4,5.0.sp2.ep302}/msgType_{D_68,8_8,F_70,G_71,9_9,V_86,W_87,X_88,j_106}.html`
5. FIX Trading Community Orchestra (FIX 4.2/4.4/FIX Latest/FIXT session) — `https://github.com/FIXTradingCommunity/orchestrations/blob/master/FIX%20Standard/`
6. OnixS "FIXT 1.1: Session Protocol" (ApplVerID precedence) — `https://www.onixs.biz/fix-dictionary/fixt1.1/section_session_protocol.html`
7. FIX Trading Community, session-level specifications — `https://www.fixtrading.org/standards/session-level-specs/`
