#include "albion/tu1_read_compare.h"
#include <iostream>
#include <stdexcept>
#include <string>

using namespace albion::data;
using namespace albion::compat;
void require(bool value, const char* label) { if (!value) throw std::runtime_error(label); }

int main(int argc, char** argv) try {
    if (argc != 2) return 2;
    auto archive = Archive::Open(std::filesystem::u8path(argv[1]));
    require(bool(archive), "open fixture");
    auto view = ArchiveReadView::Create({std::shared_ptr<const Archive>(std::move(archive.value))}, {{7, {0, 0}}});
    require(bool(view), "create fixture view");
    std::string original = "archive A";
    std::size_t checks = 0;
    auto compare = [&](ReadRequest request, std::uint32_t status, const std::string& bytes,
                       ReadAgreement wanted, ErrorCode error = ErrorCode::none,
                       std::optional<std::uint64_t> difference = {}) {
        const auto before = bytes;
        auto result = CompareTu1SynchronousRead(view.value, request,
            {status, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
        require(bool(result), "comparison rejected valid observation");
        require(result.value.agreement == wanted, "wrong agreement");
        require(result.value.native_error == error, "native error lost");
        require(result.value.first_difference == difference, "wrong first difference");
        require(bytes == before, "observation mutated");
        require(result.value.observed_size == bytes.size(), "observed size lost");
        ++checks;
    };
    compare({7, 0, 9}, 0, original, ReadAgreement::equal);
    compare({7, 2, 3}, 0, "chi", ReadAgreement::equal);
    compare({7, 9, 0}, 0, "", ReadAgreement::equal);
    for (std::size_t pos : {std::size_t{0}, std::size_t{4}, std::size_t{8}}) {
        auto altered = original;
        altered[pos] ^= 1;
        compare({7, 0, 9}, 0, altered, ReadAgreement::byte_mismatch, ErrorCode::none, pos);
    }
    compare({7, 0, 9}, 0, "archive ", ReadAgreement::length_mismatch);
    compare({7, 0, 9}, 0, "archive AA", ReadAgreement::length_mismatch);
    compare({7, 0, 9}, 0x80004005u, "", ReadAgreement::outcome_mismatch);
    compare({8, 0, 9}, 0, original, ReadAgreement::outcome_mismatch, ErrorCode::not_found);
    compare({7, 8, 9}, 0, original, ReadAgreement::outcome_mismatch, ErrorCode::out_of_range);
    compare({8, 0, 9}, 0x80004005u, "", ReadAgreement::both_failed, ErrorCode::not_found);
    compare({7, 8, 9}, 0x80004005u, "", ReadAgreement::both_failed, ErrorCode::out_of_range);
    for (auto status : {1u, 0xffffffffu, 0x80070002u}) {
        auto result = CompareTu1SynchronousRead(view.value, {7, 0, 9}, {status, nullptr, 0});
        require(result.error.code == ErrorCode::unsupported, "unknown status guessed");
        ++checks;
    }
    for (auto observed : {ObservedRead{0, nullptr, 1}, ObservedRead{0x80004005u,
             reinterpret_cast<const std::uint8_t*>(original.data()), 9}}) {
        require(CompareTu1SynchronousRead(view.value, {7, 0, 9}, observed).error.code == ErrorCode::malformed,
                "invalid observation accepted");
        ++checks;
    }
    auto empty = CompareTu1SynchronousRead(view.value, {7, 9, 0}, {0, nullptr, 0});
    require(bool(empty) && empty.value.agreement == ReadAgreement::equal, "null empty span rejected");
    ++checks;
    std::cout << checks << " comparison cases passed\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
