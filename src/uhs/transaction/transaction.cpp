// Copyright (c) 2021 MIT Digital Currency Initiative,
//                    Federal Reserve Bank of Boston
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "transaction.hpp"

#include "crypto/sha256.h"
#include "messages.hpp"
#include "util/serialization/format.hpp"
#include "util/serialization/util.hpp"

namespace cbdc::transaction {
    auto out_point::operator==(const out_point& rhs) const -> bool {
        return m_tx_id == rhs.m_tx_id && m_index == rhs.m_index;
    }

    auto out_point::operator<(const out_point& rhs) const -> bool {
        return std::tie(m_tx_id, m_index) < std::tie(rhs.m_tx_id, rhs.m_index);
    }

    out_point::out_point(const hash_t& hash, const uint64_t index)
        : m_tx_id(hash),
          m_index(index) {}

    auto output::operator==(const output& rhs) const -> bool {
        return m_witness_program_commitment == rhs.m_witness_program_commitment
            && m_id == rhs.m_id && m_nonce == rhs.m_nonce
            && m_auxiliary == rhs.m_auxiliary && m_range == rhs.m_range
            && m_consistency == rhs.m_consistency;
    }

    auto output::operator!=(const output& rhs) const -> bool {
        return !(*this == rhs);
    }

    compact_output::compact_output(const output& put)
        : m_id(put.m_id),
          m_auxiliary(put.m_auxiliary),
          m_range(put.m_range),
          m_consistency(put.m_consistency) {}

    compact_output::compact_output(const hash_t& id,
                                   const commitment_t& aux,
                                   const rangeproof_t<>& range,
                                   const signature_t& consist)
        : m_id(id),
          m_auxiliary(aux),
          m_range(range),
          m_consistency(consist) {}

    auto compact_output::operator==(const compact_output& rhs) const -> bool {
        return m_id == rhs.m_id && m_auxiliary == rhs.m_auxiliary
            && m_range == rhs.m_range && m_consistency == rhs.m_consistency;
    }

    auto compact_output::operator!=(const compact_output& rhs) const -> bool {
        return !(*this == rhs);
    }

    auto input::operator==(const input& rhs) const -> bool {
        return m_prevout == rhs.m_prevout
            && m_prevout_data == rhs.m_prevout_data;
    }

    auto input::operator!=(const input& rhs) const -> bool {
        return !(*this == rhs);
    }

    auto input::hash() const -> hash_t {
        CSHA256 sha;
        auto opt_buf = cbdc::make_buffer(this->m_prevout);
        sha.Write(opt_buf.c_ptr(), opt_buf.size());
        sha.Write(this->m_prevout_data.m_witness_program_commitment.data(),
                  sizeof(hash_t));

        hash_t result;
        sha.Finalize(result.data());

        return result;
    }

    auto full_tx::operator==(const full_tx& rhs) const -> bool {
        return rhs.m_inputs == m_inputs && rhs.m_outputs == m_outputs
            && rhs.m_witness == m_witness;
    }

    auto compact_tx_hasher::operator()(const compact_tx& tx) const noexcept
        -> size_t {
        size_t ret{};
        std::memcpy(&ret, tx.m_id.data(), sizeof(ret));
        return ret;
    }

    auto tx_id(const full_tx& tx) noexcept -> hash_t {
        CSHA256 sha;

        for(const auto& inp : tx.m_inputs) {
            sha.Write(inp.hash().data(), sizeof(hash_t));
        }

        for(const auto& out : tx.m_outputs) {
            sha.Write(out.m_witness_program_commitment.data(), sizeof(hash_t));
        }

        hash_t ret;
        sha.Finalize(ret.data());

        return ret;
    }

    auto input_from_output(const full_tx& tx, size_t i, const hash_t& txid)
        -> std::optional<input> {
        input ret;
        if(i >= tx.m_outputs.size()) {
            return std::nullopt;
        }
        ret.m_prevout_data = tx.m_outputs[i];
        ret.m_prevout.m_index = i;
        ret.m_prevout.m_tx_id = txid;

        return ret;
    }

    auto input_from_output(const full_tx& tx, size_t i)
        -> std::optional<input> {
        const auto id = tx_id(tx);
        return input_from_output(tx, i, id);
    }

    auto output_preimage(const out_point& point, const output& put)
        -> std::array<unsigned char,
                      sizeof(compact_tx::m_id) + sizeof(out_point::m_index)
                          + sizeof(output::m_witness_program_commitment)> {
        std::array<unsigned char,
                   sizeof(compact_tx::m_id) + sizeof(out_point::m_index)
                       + sizeof(output::m_witness_program_commitment)>
            buf{};

        auto* bptr = buf.data();
        std::memcpy(bptr, point.m_tx_id.data(), point.m_tx_id.size());
        bptr += sizeof(compact_tx::m_id);
        std::memcpy(bptr, &point.m_index, sizeof(point.m_index));
        bptr += sizeof(out_point::m_index);
        std::memcpy(bptr,
                    put.m_witness_program_commitment.data(),
                    put.m_witness_program_commitment.size());

        return buf;
    }

    auto output_randomness(
        std::array<unsigned char,
                   sizeof(compact_tx::m_id) + sizeof(out_point::m_index)
                       + sizeof(output::m_witness_program_commitment)> buf,
        const hash_t& nonce) -> hash_t {
        const auto bufsize = buf.size();

        CSHA256 sha;
        sha.Write(buf.data(), bufsize);
        sha.Write(nonce.data(), nonce.size());
        hash_t candidate{};
        sha.Finalize(candidate.data());

        return candidate;
    }

    auto calculate_uhs_id(
        secp256k1_context* ctx,
        random_source& rng,
        std::array<unsigned char,
                   sizeof(compact_tx::m_id) + sizeof(out_point::m_index)
                       + sizeof(output::m_witness_program_commitment)> buf,
        uint64_t value) -> std::pair<hash_t, hash_t> {
        while(true) {
            auto t = rng.random_hash();
            auto candidate = output_randomness(buf, t);
            auto c = make_xonly_commitment(ctx, value, candidate);
            if(c.has_value()) {
                return std::make_pair(c.value(), t);
            }
        }

        __builtin_unreachable();
    }

    auto calculate_uhs_id(secp256k1_context* ctx,
                          random_source& rng,
                          const out_point& point,
                          const output& put,
                          uint64_t value) -> std::pair<hash_t, hash_t> {
        auto buf = output_preimage(point, put);
        return calculate_uhs_id(ctx, rng, buf, value);
    }

    auto roll_auxiliaries(secp256k1_context* ctx,
                          random_source& rng,
                          const std::vector<hash_t>& blinds,
                          std::vector<spend_data>& out_spend_data)
        -> std::vector<secp256k1_pedersen_commitment> {
        const auto make_public = blinds.empty();
        const hash_t empty{};

        std::vector<secp256k1_pedersen_commitment> auxiliaries{};

        std::vector<hash_t> new_blinds{};
        for(uint64_t i = 0; i < out_spend_data.size() - 1; ++i) {
            while(true) {
                auto rprime = make_public ? empty : rng.random_hash();
                auto commitment
                    = commit(ctx, out_spend_data[i].m_value, rprime);
                if(commitment.has_value()) {
                    auxiliaries.push_back(commitment.value());
                    new_blinds.push_back(rprime);
                    out_spend_data[i].m_blind = rprime;
                    break;
                }
            }
        }

        if(!make_public) {
            std::vector<hash_t> allblinds{blinds};
            std::copy(new_blinds.begin(),
                      new_blinds.end(),
                      std::back_inserter(allblinds));

            std::vector<const unsigned char*> blind_ptrs;
            blind_ptrs.reserve(allblinds.size());
            for(const auto& b : allblinds) {
                blind_ptrs.push_back(b.data());
            }

            hash_t last_blind{};
            [[maybe_unused]] auto ret
                = secp256k1_pedersen_blind_sum(ctx,
                                               last_blind.data(),
                                               blind_ptrs.data(),
                                               allblinds.size(),
                                               blinds.size());
            assert(ret == 1);
            auxiliaries.push_back(
                commit(ctx, out_spend_data.back().m_value, last_blind)
                    .value());
            out_spend_data.back().m_blind = last_blind;
        } else {
            auxiliaries.push_back(
                commit(ctx, out_spend_data.back().m_value, empty).value());
            new_blinds.push_back(empty);
            out_spend_data.back().m_blind = empty;
        }

        return auxiliaries;
    }

    auto prove_output(secp256k1_context* ctx,
                      secp256k1_bulletproofs_generators* gens,
                      random_source& rng,
                      output& put,
                      const out_point& point,
                      const spend_data& out_spend_data,
                      const secp256k1_pedersen_commitment* auxiliary) -> bool {
        const auto out_preimage = output_preimage(point, put);
        auto [uhs, nonce]
            = calculate_uhs_id(ctx, rng, out_preimage, out_spend_data.m_value);

        put.m_id = uhs;
        put.m_nonce = nonce;

        // manually derive the secret key
        auto esk = output_randomness(out_preimage, nonce);
        auto rprime = out_spend_data.m_blind;
        [[maybe_unused]] auto ret
            = secp256k1_ec_seckey_negate(ctx, rprime.data());
        // fails when rprime == 0 (which is fine)
        ret = secp256k1_ec_seckey_tweak_add(ctx, esk.data(), rprime.data());
        assert(ret == 1);

        secp256k1_keypair kp{};
        ret = secp256k1_keypair_create(ctx, &kp, esk.data());
        assert(ret == 1);

        auto consistency = signature_t{};
        unsigned char consist_contents[32] = "consistent proof";
        ret = secp256k1_schnorrsig_sign(ctx,
                                        consistency.data(),
                                        consist_contents,
                                        &kp,
                                        nullptr,
                                        nullptr);
        assert(ret == 1);

        put.m_consistency = consistency;

        rangeproof_t<> range{};
        size_t rangelen = range.size();
        ret = secp256k1_bulletproofs_rangeproof_uncompressed_prove(
            ctx,
            gens,
            secp256k1_generator_h,
            range.data(),
            &rangelen,
            64, // 2^64-1
            out_spend_data.m_value,
            0,
            auxiliary, // the auxiliary commitment for this output
            out_spend_data.m_blind.data(),
            rng.random_hash().data(),
            nullptr, // enc_data
            nullptr, // extra_commit
            0     // extra_commit length
        );
        assert(ret == 1);

        put.m_range = range;
        put.m_auxiliary = serialize_commitment(ctx, *auxiliary);

        return true;
    }

    auto sign_nonces(
        secp256k1_context* ctx,
        std::vector<unsigned char> nonces,
        const std::vector<std::pair<privkey_t, pubkey_t>>& spending_keys)
        -> std::vector<signature_t> {
        std::vector<signature_t> noncesigs{};
        noncesigs.reserve(spending_keys.size());
        for(size_t i = 0; i < spending_keys.size(); ++i) {
            const auto& [sk, pk] = spending_keys[i];
            secp256k1_keypair spending_kp{};
            [[maybe_unused]] auto ret
                = secp256k1_keypair_create(ctx, &spending_kp, sk.data());
            assert(ret == 1);

            signature_t noncesig{};
            ret = secp256k1_schnorrsig_sign(ctx,
                                            noncesig.data(),
                                            nonces.data(),
                                            &spending_kp,
                                            nullptr,
                                            nullptr);
            assert(ret == 1);

            noncesigs[i] = noncesig;
        }

        return noncesigs;
    }

    auto
    add_proof(secp256k1_context* ctx,
              secp256k1_bulletproofs_generators* gens,
              random_source& rng,
              full_tx& tx,
              const std::vector<std::pair<privkey_t, pubkey_t>>& spending_keys)
        -> bool {
        std::vector<hash_t> blinds{};
        for(const auto& inp : tx.m_inputs) {
            auto spend_data = inp.m_spend_data;
            if(!spend_data.has_value() || spend_data.value().m_value == 0) {
                // No input spend data or there was a zero-valued input
                return false;
            }
            blinds.push_back(spend_data.value().m_blind);
        }

        if(!tx.m_out_spend_data.has_value()) {
            // No output spend data
            return false;
        }
        const auto& out_spend_data = tx.m_out_spend_data.value();
        for(const auto& spend_data : out_spend_data) {
            if(spend_data.m_value == 0) {
                // There was a zero-valued output
                return false;
            }
        }

        auto auxiliaries
            = roll_auxiliaries(ctx, rng, blinds, tx.m_out_spend_data.value());

        const auto txid = tx_id(tx);

        for(uint64_t i = 0; i < tx.m_outputs.size(); ++i) {
            [[maybe_unused]] auto res = prove_output(
                ctx,
                gens,
                rng,
                tx.m_outputs[i],
                input_from_output(tx, i, txid).value().m_prevout,
                tx.m_out_spend_data.value()[i],
                &auxiliaries[i]);
            if(!res) {
                return false;
            }
        }

        // todo: blind spend-required data after adding proof
        //       (unclear quite when this should happen)
        // todo: same for m_out_spend_data?
        // for(auto& inp : tx.m_inputs) {
        //    inp.m_spend_data = std::nullopt;
        //}

        std::vector<unsigned char> nonces{};
        for(const auto& put : tx.m_outputs) {
            const auto& nonce = put.m_nonce;
            std::copy(nonce.begin(), nonce.end(), std::back_inserter(nonces));
        }

        tx.m_tx_proofs = {sign_nonces(ctx, nonces, spending_keys)};

        return true;
    }

    auto compact_tx::operator==(const compact_tx& tx) const noexcept -> bool {
        return m_id == tx.m_id;
    }

    compact_tx::compact_tx(const full_tx& tx) {
        m_id = tx_id(tx);
        for(const auto& inp : tx.m_inputs) {
            m_inputs.push_back(inp.m_prevout_data.m_id);
        }
        for(const auto& put : tx.m_outputs) {
            m_outputs.emplace_back(put);
        }
    }
}
