/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file block.cpp
 * @author Christoph Jentzsch <cj@ethdev.com>
 * @date 2015
 * block test functions.
 */

#include <boost/filesystem.hpp>
#include <libdevcrypto/FileSystem.h>
#include <libdevcore/TransientDirectory.h>
#include <libethereum/CanonBlockChain.h>
#include <test/TestHelper.h>

using namespace std;
using namespace json_spirit;
using namespace dev;
using namespace dev::eth;

namespace dev {  namespace test {

BlockInfo constructBlock(mObject& _o);
bytes createBlockRLPFromFields(mObject& _tObj);
RLPStream createFullBlockFromHeader(BlockInfo const& _bi, bytes const& _txs = RLPEmptyList, bytes const& _uncles = RLPEmptyList);

mArray writeTransactionsToJson(Transactions const& txs);
mObject writeBlockHeaderToJson(mObject& _o, BlockInfo const& _bi);
void overwriteBlockHeader(BlockInfo& _current_BlockHeader, mObject& _blObj);
BlockInfo constructBlock(mObject& _o);
void updatePoW(BlockInfo& _bi);
mArray importUncles(mObject const& blObj, vector<BlockInfo>& vBiUncles, vector<BlockInfo> const& vBiBlocks);

void doBlockchainTests(json_spirit::mValue& _v, bool _fillin)
{
	for (auto& i: _v.get_obj())
	{
		mObject& o = i.second.get_obj();
		if (test::Options::get().singleTest && test::Options::get().singleTestName != i.first)
		{
			o.clear();
			continue;
		}

		cerr << i.first << endl;
		BOOST_REQUIRE(o.count("genesisBlockHeader"));
		BlockInfo biGenesisBlock = constructBlock(o["genesisBlockHeader"].get_obj());

		BOOST_REQUIRE(o.count("pre"));
		ImportTest importer(o["pre"].get_obj());
		TransientDirectory td_stateDB_tmp;
		State trueState(OverlayDB(State::openDB(td_stateDB_tmp.path())), BaseState::Empty, biGenesisBlock.coinbaseAddress);

		//Imported blocks from the start
		typedef std::vector<bytes> uncleList;
		typedef std::pair<bytes, uncleList> blockSet;
		std::vector<blockSet> blockSets;

		importer.importState(o["pre"].get_obj(), trueState);
		o["pre"] = fillJsonWithState(trueState);
		trueState.commit();

		if (_fillin)
			biGenesisBlock.stateRoot = trueState.rootHash();
		else
			BOOST_CHECK_MESSAGE(biGenesisBlock.stateRoot == trueState.rootHash(), "root hash does not match");

		if (_fillin)
		{
			// find new valid nonce
			updatePoW(biGenesisBlock);

			//update genesis block in json file
			writeBlockHeaderToJson(o["genesisBlockHeader"].get_obj(), biGenesisBlock);
		}

		// create new "genesis" block
		RLPStream rlpGenesisBlock = createFullBlockFromHeader(biGenesisBlock);
		biGenesisBlock.verifyInternals(&rlpGenesisBlock.out());
		o["genesisRLP"] = toHex(rlpGenesisBlock.out(), 2, HexPrefix::Add);

		// construct true blockchain
		TransientDirectory td;
		BlockChain trueBc(rlpGenesisBlock.out(), td.path(), WithExisting::Kill);

		if (_fillin)
		{
			BOOST_REQUIRE(o.count("blocks"));
			mArray blArray;

			blockSet genesis;
			genesis.first = rlpGenesisBlock.out();
			genesis.second = uncleList();
			blockSets.push_back(genesis);
			vector<BlockInfo> vBiBlocks;
			vBiBlocks.push_back(biGenesisBlock);

			size_t importBlockNumber = 0;
			for (auto const& bl: o["blocks"].get_array())
			{
				mObject blObj = bl.get_obj();
				if (blObj.count("blocknumber") > 0)
					importBlockNumber = std::max((int)toInt(blObj["blocknumber"]), 1);
				else
					importBlockNumber++;

				//each time construct a new blockchain up to importBlockNumber (to generate next block header)
				vBiBlocks.clear();
				vBiBlocks.push_back(biGenesisBlock);

				TransientDirectory td_stateDB, td_bc;
				BlockChain bc(rlpGenesisBlock.out(), td_bc.path(), WithExisting::Kill);
				State state(OverlayDB(State::openDB(td_stateDB.path())), BaseState::Empty, biGenesisBlock.coinbaseAddress);
				importer.importState(o["pre"].get_obj(), state);
				state.commit();

				for (size_t i = 1; i < importBlockNumber; i++) //0 block is genesis
				{
					BlockQueue uncleQueue;
					uncleList uncles = blockSets.at(i).second;
					for (size_t j = 0; j < uncles.size(); j++)
						uncleQueue.import(&uncles.at(j), bc);

					const bytes block = blockSets.at(i).first;
					bc.sync(uncleQueue, state.db(), 4);
					bc.attemptImport(block, state.db());
					vBiBlocks.push_back(BlockInfo(block));

					state.sync(bc);
				}

				// get txs
				TransactionQueue txs;
				ZeroGasPricer gp;
				BOOST_REQUIRE(blObj.count("transactions"));
				for (auto const& txObj: blObj["transactions"].get_array())
				{
					mObject tx = txObj.get_obj();
					importer.importTransaction(tx);
					if (txs.import(importer.m_transaction.rlp()) != ImportResult::Success)
						cnote << "failed importing transaction\n";
				}

				//get uncles
				vector<BlockInfo> vBiUncles;
				blObj["uncleHeaders"] = importUncles(blObj, vBiUncles, vBiBlocks);

				BlockQueue uncleBlockQueue;
				uncleList uncleBlockQueueList;
				cnote << "import uncle in blockQueue";
				for (size_t i = 0; i < vBiUncles.size(); i++)
				{
					RLPStream uncle = createFullBlockFromHeader(vBiUncles.at(i));
					try
					{
						uncleBlockQueue.import(&uncle.out(), bc);
						uncleBlockQueueList.push_back(uncle.out());
					}
					catch(...)
					{
						cnote << "error in importing uncle! This produces an invalid block (May be by purpose for testing).";
					}
				} 

				bc.sync(uncleBlockQueue, state.db(), 4);
				state.commitToMine(bc);

				try
				{
					state.sync(bc);
					state.sync(bc, txs, gp);
					mine(state, bc);
				}
				catch (Exception const& _e)
				{
					cnote << "state sync or mining did throw an exception: " << diagnostic_information(_e);
					return;
				}
				catch (std::exception const& _e)
				{
					cnote << "state sync or mining did throw an exception: " << _e.what();
					return;
				}

				blObj["rlp"] = toHex(state.blockData(), 2, HexPrefix::Add);

				//get valid transactions
				Transactions txList;
				for (auto const& txi: txs.transactions())
					txList.push_back(txi.second);
				blObj["transactions"] = writeTransactionsToJson(txList);

				BlockInfo current_BlockHeader = state.info();
				if (blObj.count("blockHeader"))
					overwriteBlockHeader(current_BlockHeader, blObj);

				// write block header
				mObject oBlockHeader;
				writeBlockHeaderToJson(oBlockHeader, current_BlockHeader);
				blObj["blockHeader"] = oBlockHeader;
				vBiBlocks.push_back(current_BlockHeader);

				// compare blocks from state and from rlp
				RLPStream txStream;
				txStream.appendList(txList.size());
				for (unsigned i = 0; i < txList.size(); ++i)
				{
					RLPStream txrlp;
					txList[i].streamRLP(txrlp);
					txStream.appendRaw(txrlp.out());
				}

				RLPStream uncleStream;
				uncleStream.appendList(vBiUncles.size());
				for (unsigned i = 0; i < vBiUncles.size(); ++i)
				{
					RLPStream uncleRlp;
					vBiUncles[i].streamRLP(uncleRlp, WithNonce);
					uncleStream.appendRaw(uncleRlp.out());
				}

				RLPStream block2 = createFullBlockFromHeader(current_BlockHeader, txStream.out(), uncleStream.out());

				blObj["rlp"] = toHex(block2.out(), 2, HexPrefix::Add);

				if (sha3(RLP(state.blockData())[0].data()) != sha3(RLP(block2.out())[0].data()))
					cnote << "block header mismatch\n";

				if (sha3(RLP(state.blockData())[1].data()) != sha3(RLP(block2.out())[1].data()))
					cnote << "txs mismatch\n";

				if (sha3(RLP(state.blockData())[2].data()) != sha3(RLP(block2.out())[2].data()))
					cnote << "uncle list mismatch\n" << RLP(state.blockData())[2].data() << "\n" << RLP(block2.out())[2].data();

				try
				{
					state.sync(bc);
					bc.import(block2.out(), state.db());
					state.sync(bc);
					state.commit();

					//there we get new blockchain status in state which could have more difficulty than we have in trueState
					//attempt to import new block to the true blockchain
					trueBc.sync(uncleBlockQueue, trueState.db(), 4);
					trueBc.attemptImport(block2.out(), trueState.db());
					trueState.sync(trueBc);

					blockSet newBlock;
					newBlock.first = block2.out();
					newBlock.second = uncleBlockQueueList;
					if (importBlockNumber < blockSets.size())
					{
						//make new correct history of imported blocks
						blockSets[importBlockNumber] = newBlock;
						for (size_t i = importBlockNumber + 1; i < blockSets.size(); i++)
							blockSets.pop_back();
					}
					else
						blockSets.push_back(newBlock);
				}
				// if exception is thrown, RLP is invalid and no blockHeader, Transaction list, or Uncle list should be given
				catch (...)
				{
					cnote << "block is invalid!\n";
					blObj.erase(blObj.find("blockHeader"));
					blObj.erase(blObj.find("uncleHeaders"));
					blObj.erase(blObj.find("transactions"));
				}
				blArray.push_back(blObj);
				this_thread::sleep_for(chrono::seconds(1));
			} //for blocks

			if (o.count("expect") > 0)
			{
				stateOptionsMap expectStateMap;
				State stateExpect(OverlayDB(), BaseState::Empty, biGenesisBlock.coinbaseAddress);
				importer.importState(o["expect"].get_obj(), stateExpect, expectStateMap);
				ImportTest::checkExpectedState(stateExpect, trueState, expectStateMap, Options::get().checkState ? WhenError::Throw : WhenError::DontThrow);
				o.erase(o.find("expect"));
			}

			o["blocks"] = blArray;
			o["postState"] = fillJsonWithState(trueState);
			o["lastblockhash"] = toString(trueBc.info().hash());

			//make all values hex in pre section
			State prestate(OverlayDB(), BaseState::Empty, biGenesisBlock.coinbaseAddress);
			importer.importState(o["pre"].get_obj(), prestate);
			o["pre"] = fillJsonWithState(prestate);
		}//_fillin

		else
		{
			for (auto const& bl: o["blocks"].get_array())
			{
				bool importedAndBest = true;
				mObject blObj = bl.get_obj();
				bytes blockRLP;
				try
				{
					blockRLP = importByteArray(blObj["rlp"].get_str());
					trueState.sync(trueBc);
					trueBc.import(blockRLP, trueState.db());
					if (trueBc.info() != BlockInfo(blockRLP))
						importedAndBest  = false;
					trueState.sync(trueBc);
				}
				// if exception is thrown, RLP is invalid and no blockHeader, Transaction list, or Uncle list should be given
				catch (Exception const& _e)
				{
					cnote << "state sync or block import did throw an exception: " << diagnostic_information(_e);
					BOOST_CHECK(blObj.count("blockHeader") == 0);
					BOOST_CHECK(blObj.count("transactions") == 0);
					BOOST_CHECK(blObj.count("uncleHeaders") == 0);
					continue;
				}
				catch (std::exception const& _e)
				{
					cnote << "state sync or block import did throw an exception: " << _e.what();
					BOOST_CHECK(blObj.count("blockHeader") == 0);
					BOOST_CHECK(blObj.count("transactions") == 0);
					BOOST_CHECK(blObj.count("uncleHeaders") == 0);
					continue;
				}
				catch (...)
				{
					cnote << "state sync or block import did throw an exception\n";
					BOOST_CHECK(blObj.count("blockHeader") == 0);
					BOOST_CHECK(blObj.count("transactions") == 0);
					BOOST_CHECK(blObj.count("uncleHeaders") == 0);
					continue;
				}

				BOOST_REQUIRE(blObj.count("blockHeader"));

				mObject tObj = blObj["blockHeader"].get_obj();
				BlockInfo blockHeaderFromFields;
				const bytes c_rlpBytesBlockHeader = createBlockRLPFromFields(tObj);
				const RLP c_blockHeaderRLP(c_rlpBytesBlockHeader);
				blockHeaderFromFields.populateFromHeader(c_blockHeaderRLP, IgnoreNonce);

				BlockInfo blockFromRlp = trueBc.info();

				if (importedAndBest)
				{
					//Check the fields restored from RLP to original fields
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.headerHash(WithNonce) == blockFromRlp.headerHash(WithNonce), "hash in given RLP not matching the block hash!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.parentHash == blockFromRlp.parentHash, "parentHash in given RLP not matching the block parentHash!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.sha3Uncles == blockFromRlp.sha3Uncles, "sha3Uncles in given RLP not matching the block sha3Uncles!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.coinbaseAddress == blockFromRlp.coinbaseAddress,"coinbaseAddress in given RLP not matching the block coinbaseAddress!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.stateRoot == blockFromRlp.stateRoot, "stateRoot in given RLP not matching the block stateRoot!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.transactionsRoot == blockFromRlp.transactionsRoot, "transactionsRoot in given RLP not matching the block transactionsRoot!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.receiptsRoot == blockFromRlp.receiptsRoot, "receiptsRoot in given RLP not matching the block receiptsRoot!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.logBloom == blockFromRlp.logBloom, "logBloom in given RLP not matching the block logBloom!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.difficulty == blockFromRlp.difficulty, "difficulty in given RLP not matching the block difficulty!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.number == blockFromRlp.number, "number in given RLP not matching the block number!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.gasLimit == blockFromRlp.gasLimit,"gasLimit in given RLP not matching the block gasLimit!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.gasUsed == blockFromRlp.gasUsed, "gasUsed in given RLP not matching the block gasUsed!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.timestamp == blockFromRlp.timestamp, "timestamp in given RLP not matching the block timestamp!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.extraData == blockFromRlp.extraData, "extraData in given RLP not matching the block extraData!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.mixHash == blockFromRlp.mixHash, "mixHash in given RLP not matching the block mixHash!");
					BOOST_CHECK_MESSAGE(blockHeaderFromFields.nonce == blockFromRlp.nonce, "nonce in given RLP not matching the block nonce!");

					BOOST_CHECK_MESSAGE(blockHeaderFromFields == blockFromRlp, "However, blockHeaderFromFields != blockFromRlp!");

					//Check transaction list

					Transactions txsFromField;

					for (auto const& txObj: blObj["transactions"].get_array())
					{
						mObject tx = txObj.get_obj();

						BOOST_REQUIRE(tx.count("nonce"));
						BOOST_REQUIRE(tx.count("gasPrice"));
						BOOST_REQUIRE(tx.count("gasLimit"));
						BOOST_REQUIRE(tx.count("to"));
						BOOST_REQUIRE(tx.count("value"));
						BOOST_REQUIRE(tx.count("v"));
						BOOST_REQUIRE(tx.count("r"));
						BOOST_REQUIRE(tx.count("s"));
						BOOST_REQUIRE(tx.count("data"));

						try
						{
							Transaction t(createRLPStreamFromTransactionFields(tx).out(), CheckTransaction::Everything);
							txsFromField.push_back(t);
						}
						catch (Exception const& _e)
						{
							BOOST_ERROR("Failed transaction constructor with Exception: " << diagnostic_information(_e));
						}
						catch (exception const& _e)
						{
							cnote << _e.what();
						}
					}

					Transactions txsFromRlp;
					RLP root(blockRLP);
					for (auto const& tr: root[1])
					{
						Transaction tx(tr.data(), CheckTransaction::Everything);
						txsFromRlp.push_back(tx);
					}

					BOOST_CHECK_MESSAGE(txsFromRlp.size() == txsFromField.size(), "transaction list size does not match");

					for (size_t i = 0; i < txsFromField.size(); ++i)
					{
						BOOST_CHECK_MESSAGE(txsFromField[i].data() == txsFromRlp[i].data(), "transaction data in rlp and in field do not match");
						BOOST_CHECK_MESSAGE(txsFromField[i].gas() == txsFromRlp[i].gas(), "transaction gasLimit in rlp and in field do not match");
						BOOST_CHECK_MESSAGE(txsFromField[i].gasPrice() == txsFromRlp[i].gasPrice(), "transaction gasPrice in rlp and in field do not match");
						BOOST_CHECK_MESSAGE(txsFromField[i].nonce() == txsFromRlp[i].nonce(), "transaction nonce in rlp and in field do not match");
						BOOST_CHECK_MESSAGE(txsFromField[i].signature().r == txsFromRlp[i].signature().r, "transaction r in rlp and in field do not match");
						BOOST_CHECK_MESSAGE(txsFromField[i].signature().s == txsFromRlp[i].signature().s, "transaction s in rlp and in field do not match");
						BOOST_CHECK_MESSAGE(txsFromField[i].signature().v == txsFromRlp[i].signature().v, "transaction v in rlp and in field do not match");
						BOOST_CHECK_MESSAGE(txsFromField[i].receiveAddress() == txsFromRlp[i].receiveAddress(), "transaction receiveAddress in rlp and in field do not match");
						BOOST_CHECK_MESSAGE(txsFromField[i].value() == txsFromRlp[i].value(), "transaction receiveAddress in rlp and in field do not match");

						BOOST_CHECK_MESSAGE(txsFromField[i] == txsFromRlp[i], "transactions from  rlp and transaction from field do not match");
						BOOST_CHECK_MESSAGE(txsFromField[i].rlp() == txsFromRlp[i].rlp(), "transactions rlp do not match");
					}

					// check uncle list

					// uncles from uncle list field
					vector<BlockInfo> uBlHsFromField;
					if (blObj["uncleHeaders"].type() != json_spirit::null_type)
						for (auto const& uBlHeaderObj: blObj["uncleHeaders"].get_array())
						{
							mObject uBlH = uBlHeaderObj.get_obj();
							BOOST_REQUIRE(uBlH.size() == 16);
							bytes uncleRLP = createBlockRLPFromFields(uBlH);
							const RLP c_uRLP(uncleRLP);
							BlockInfo uncleBlockHeader;
							try
							{
								uncleBlockHeader.populateFromHeader(c_uRLP);
							}
							catch(...)
							{
								BOOST_ERROR("invalid uncle header");
							}
							uBlHsFromField.push_back(uncleBlockHeader);
						}

					// uncles from block RLP
					vector<BlockInfo> uBlHsFromRlp;
					for	(auto const& uRLP: root[2])
					{
						BlockInfo uBl;
						uBl.populateFromHeader(uRLP);
						uBlHsFromRlp.push_back(uBl);
					}

					BOOST_REQUIRE_EQUAL(uBlHsFromField.size(), uBlHsFromRlp.size());

					for (size_t i = 0; i < uBlHsFromField.size(); ++i)
						BOOST_CHECK_MESSAGE(uBlHsFromField[i] == uBlHsFromRlp[i], "block header in rlp and in field do not match");
				}//importedAndBest
			}//all blocks

			BOOST_REQUIRE(o.count("lastblockhash") > 0);
			BOOST_CHECK_MESSAGE(toString(trueBc.info().hash()) == o["lastblockhash"].get_str(),
					"Boost check: " + i.first + " lastblockhash does not match " + toString(trueBc.info().hash()) + " expected: " + o["lastblockhash"].get_str());
		}
	}
}

// helping functions

mArray importUncles(mObject const& blObj, vector<BlockInfo>& vBiUncles, vector<BlockInfo> const& vBiBlocks)
{
	// write uncle list
	mArray aUncleList;
	mObject uncleHeaderObj_pre;

	for (auto const& uHObj: blObj.at("uncleHeaders").get_array())
	{
		mObject uncleHeaderObj = uHObj.get_obj();
		if (uncleHeaderObj.count("sameAsPreviousSibling"))
		{
			writeBlockHeaderToJson(uncleHeaderObj_pre, vBiUncles[vBiUncles.size()-1]);
			aUncleList.push_back(uncleHeaderObj_pre);
			vBiUncles.push_back(vBiUncles[vBiUncles.size()-1]);
			continue;
		}

		if (uncleHeaderObj.count("sameAsBlock"))
		{
			writeBlockHeaderToJson(uncleHeaderObj_pre, vBiBlocks[(size_t)toInt(uncleHeaderObj["sameAsBlock"])]);
			aUncleList.push_back(uncleHeaderObj_pre);
			vBiUncles.push_back(vBiBlocks[(size_t)toInt(uncleHeaderObj["sameAsBlock"])]);
			continue;
		}
		string overwrite = "false";
		if (uncleHeaderObj.count("overwriteAndRedoPoW"))
		{
			overwrite = uncleHeaderObj["overwriteAndRedoPoW"].get_str();
			uncleHeaderObj.erase("overwriteAndRedoPoW");
		}

		BlockInfo uncleBlockFromFields = constructBlock(uncleHeaderObj);

		// make uncle header valid
		uncleBlockFromFields.timestamp = (u256)time(0);
		cnote << "uncle block n = " << toString(uncleBlockFromFields.number);
		if (vBiBlocks.size() > 2)
		{
			if (uncleBlockFromFields.number - 1 < vBiBlocks.size())
				uncleBlockFromFields.populateFromParent(vBiBlocks[(size_t)uncleBlockFromFields.number - 1]);
			else
				uncleBlockFromFields.populateFromParent(vBiBlocks[vBiBlocks.size() - 2]);
		}
		else
			continue;

		if (overwrite != "false")
		{
			uncleBlockFromFields.difficulty = overwrite == "difficulty" ? toInt(uncleHeaderObj["difficulty"]) : uncleBlockFromFields.difficulty;
			uncleBlockFromFields.gasLimit = overwrite == "gasLimit" ? toInt(uncleHeaderObj["gasLimit"]) : uncleBlockFromFields.gasLimit;
			uncleBlockFromFields.gasUsed = overwrite == "gasUsed" ? toInt(uncleHeaderObj["gasUsed"]) : uncleBlockFromFields.gasUsed;
			uncleBlockFromFields.parentHash = overwrite == "parentHash" ? h256(uncleHeaderObj["parentHash"].get_str()) : uncleBlockFromFields.parentHash;
			uncleBlockFromFields.stateRoot = overwrite == "stateRoot" ? h256(uncleHeaderObj["stateRoot"].get_str()) : uncleBlockFromFields.stateRoot;
			if (overwrite == "timestamp")
			{
				uncleBlockFromFields.timestamp = toInt(uncleHeaderObj["timestamp"]);
				uncleBlockFromFields.difficulty = uncleBlockFromFields.calculateDifficulty(vBiBlocks[(size_t)uncleBlockFromFields.number - 1]);
			}
		}

		updatePoW(uncleBlockFromFields);
		writeBlockHeaderToJson(uncleHeaderObj, uncleBlockFromFields);

		aUncleList.push_back(uncleHeaderObj);
		vBiUncles.push_back(uncleBlockFromFields);

		uncleHeaderObj_pre = uncleHeaderObj;
	} //for blObj["uncleHeaders"].get_array()

	return aUncleList;
}

bytes createBlockRLPFromFields(mObject& _tObj)
{
	RLPStream rlpStream;
	rlpStream.appendList(_tObj.count("hash") > 0 ? (_tObj.size() - 1) : _tObj.size());

	if (_tObj.count("parentHash"))
		rlpStream << importByteArray(_tObj["parentHash"].get_str());

	if (_tObj.count("uncleHash"))
		rlpStream << importByteArray(_tObj["uncleHash"].get_str());

	if (_tObj.count("coinbase"))
		rlpStream << importByteArray(_tObj["coinbase"].get_str());

	if (_tObj.count("stateRoot"))
		rlpStream << importByteArray(_tObj["stateRoot"].get_str());

	if (_tObj.count("transactionsTrie"))
		rlpStream << importByteArray(_tObj["transactionsTrie"].get_str());

	if (_tObj.count("receiptTrie"))
		rlpStream << importByteArray(_tObj["receiptTrie"].get_str());

	if (_tObj.count("bloom"))
		rlpStream << importByteArray(_tObj["bloom"].get_str());

	if (_tObj.count("difficulty"))
		rlpStream << bigint(_tObj["difficulty"].get_str());

	if (_tObj.count("number"))
		rlpStream << bigint(_tObj["number"].get_str());

	if (_tObj.count("gasLimit"))
		rlpStream << bigint(_tObj["gasLimit"].get_str());

	if (_tObj.count("gasUsed"))
		rlpStream << bigint(_tObj["gasUsed"].get_str());

	if (_tObj.count("timestamp"))
		rlpStream << bigint(_tObj["timestamp"].get_str());

	if (_tObj.count("extraData"))
		rlpStream << fromHex(_tObj["extraData"].get_str());

	if (_tObj.count("mixHash"))
		rlpStream << importByteArray(_tObj["mixHash"].get_str());

	if (_tObj.count("nonce"))
		rlpStream << importByteArray(_tObj["nonce"].get_str());

	return rlpStream.out();
}

void overwriteBlockHeader(BlockInfo& _header, mObject& _blObj)
{
	auto ho = _blObj["blockHeader"].get_obj();
	if (ho.size() != 14)
	{
		BlockInfo tmp = _header;
		if (ho.count("parentHash"))
			tmp.parentHash = h256(ho["parentHash"].get_str());
		if (ho.count("uncleHash"))
			tmp.sha3Uncles = h256(ho["uncleHash"].get_str());
		if (ho.count("coinbase"))
			tmp.coinbaseAddress = Address(ho["coinbase"].get_str());
		if (ho.count("stateRoot"))
			tmp.stateRoot = h256(ho["stateRoot"].get_str());
		if (ho.count("transactionsTrie"))
			tmp.transactionsRoot = h256(ho["transactionsTrie"].get_str());
		if (ho.count("receiptTrie"))
			tmp.receiptsRoot = h256(ho["receiptTrie"].get_str());
		if (ho.count("bloom"))
			tmp.logBloom = LogBloom(ho["bloom"].get_str());
		if (ho.count("difficulty"))
			tmp.difficulty = toInt(ho["difficulty"]);
		if (ho.count("number"))
			tmp.number = toInt(ho["number"]);
		if (ho.count("gasLimit"))
			tmp.gasLimit = toInt(ho["gasLimit"]);
		if (ho.count("gasUsed"))
			tmp.gasUsed = toInt(ho["gasUsed"]);
		if (ho.count("timestamp"))
			tmp.timestamp = toInt(ho["timestamp"]);
		if (ho.count("extraData"))
			tmp.extraData = importByteArray(ho["extraData"].get_str());
		if (ho.count("mixHash"))
			tmp.mixHash = h256(ho["mixHash"].get_str());
		tmp.noteDirty();

		// find new valid nonce
		if (tmp != _header)
		{
			mine(tmp);
			_header = tmp;
		}
	}
	else
	{
		// take the blockheader as is
		const bytes c_blockRLP = createBlockRLPFromFields(ho);
		const RLP c_bRLP(c_blockRLP);
		_header.populateFromHeader(c_bRLP, IgnoreNonce);
	}
}

BlockInfo constructBlock(mObject& _o)
{
	BlockInfo ret;
	try
	{
		// construct genesis block
		const bytes c_blockRLP = createBlockRLPFromFields(_o);
		const RLP c_bRLP(c_blockRLP);
		ret.populateFromHeader(c_bRLP, IgnoreNonce);
	}
	catch (Exception const& _e)
	{
		cnote << "block population did throw an exception: " << diagnostic_information(_e);
	}
	catch (std::exception const& _e)
	{
		BOOST_ERROR("Failed block population with Exception: " << _e.what());
	}
	catch(...)
	{
		BOOST_ERROR("block population did throw an unknown exception\n");
	}
	return ret;
}

void updatePoW(BlockInfo& _bi)
{
	mine(_bi);
	_bi.noteDirty();
}

mArray writeTransactionsToJson(Transactions const& txs)
{
	mArray txArray;
	for (auto const& txi: txs)
	{
		mObject txObject = fillJsonWithTransaction(txi);
		txArray.push_back(txObject);
	}
	return txArray;
}

mObject writeBlockHeaderToJson(mObject& _o, BlockInfo const& _bi)
{
	_o["parentHash"] = toString(_bi.parentHash);
	_o["uncleHash"] = toString(_bi.sha3Uncles);
	_o["coinbase"] = toString(_bi.coinbaseAddress);
	_o["stateRoot"] = toString(_bi.stateRoot);
	_o["transactionsTrie"] = toString(_bi.transactionsRoot);
	_o["receiptTrie"] = toString(_bi.receiptsRoot);
	_o["bloom"] = toString(_bi.logBloom);
	_o["difficulty"] = toCompactHex(_bi.difficulty, HexPrefix::Add, 1);
	_o["number"] = toCompactHex(_bi.number, HexPrefix::Add, 1);
	_o["gasLimit"] = toCompactHex(_bi.gasLimit, HexPrefix::Add, 1);
	_o["gasUsed"] = toCompactHex(_bi.gasUsed, HexPrefix::Add, 1);
	_o["timestamp"] = toCompactHex(_bi.timestamp, HexPrefix::Add, 1);
	_o["extraData"] = toHex(_bi.extraData, 2, HexPrefix::Add);
	_o["mixHash"] = toString(_bi.mixHash);
	_o["nonce"] = toString(_bi.nonce);
	_o["hash"] = toString(_bi.hash());
	return _o;
}

RLPStream createFullBlockFromHeader(BlockInfo const& _bi, bytes const& _txs, bytes const& _uncles)
{
	RLPStream rlpStream;
	_bi.streamRLP(rlpStream, WithNonce);

	RLPStream ret(3);
	ret.appendRaw(rlpStream.out());
	ret.appendRaw(_txs);
	ret.appendRaw(_uncles);

	return ret;
}

} }// Namespace Close

BOOST_AUTO_TEST_SUITE(BlockChainTests)

BOOST_AUTO_TEST_CASE(bcForkBlockTest)
{
	dev::test::executeTests("bcForkBlockTest", "/BlockTests",dev::test::getFolder(__FILE__) + "/BlockTestsFiller", dev::test::doBlockchainTests);
}

BOOST_AUTO_TEST_CASE(bcTotalDifficultyTest)
{
	dev::test::executeTests("bcTotalDifficultyTest", "/BlockTests",dev::test::getFolder(__FILE__) + "/BlockTestsFiller", dev::test::doBlockchainTests);
}

BOOST_AUTO_TEST_CASE(bcInvalidRLPTest)
{
	dev::test::executeTests("bcInvalidRLPTest", "/BlockTests",dev::test::getFolder(__FILE__) + "/BlockTestsFiller", dev::test::doBlockchainTests);
}

BOOST_AUTO_TEST_CASE(bcRPC_API_Test)
{
	dev::test::executeTests("bcRPC_API_Test", "/BlockTests",dev::test::getFolder(__FILE__) + "/BlockTestsFiller", dev::test::doBlockchainTests);
}

BOOST_AUTO_TEST_CASE(bcValidBlockTest)
{
	dev::test::executeTests("bcValidBlockTest", "/BlockTests",dev::test::getFolder(__FILE__) + "/BlockTestsFiller", dev::test::doBlockchainTests);
}

BOOST_AUTO_TEST_CASE(bcInvalidHeaderTest)
{
	dev::test::executeTests("bcInvalidHeaderTest", "/BlockTests",dev::test::getFolder(__FILE__) + "/BlockTestsFiller", dev::test::doBlockchainTests);
}

BOOST_AUTO_TEST_CASE(bcUncleTest)
{
	dev::test::executeTests("bcUncleTest", "/BlockTests",dev::test::getFolder(__FILE__) + "/BlockTestsFiller", dev::test::doBlockchainTests);
}

BOOST_AUTO_TEST_CASE(bcUncleHeaderValiditiy)
{
	dev::test::executeTests("bcUncleHeaderValiditiy", "/BlockTests",dev::test::getFolder(__FILE__) + "/BlockTestsFiller", dev::test::doBlockchainTests);
}

BOOST_AUTO_TEST_CASE(bcGasPricerTest)
{
	dev::test::executeTests("bcGasPricerTest", "/BlockTests",dev::test::getFolder(__FILE__) + "/BlockTestsFiller", dev::test::doBlockchainTests);
}

BOOST_AUTO_TEST_CASE(bcWalletTest)
{
	dev::test::executeTests("bcWalletTest", "/BlockTests",dev::test::getFolder(__FILE__) + "/BlockTestsFiller", dev::test::doBlockchainTests);
}

BOOST_AUTO_TEST_CASE(userDefinedFile)
{
	dev::test::userDefinedTest(dev::test::doBlockchainTests);
}

BOOST_AUTO_TEST_SUITE_END()
