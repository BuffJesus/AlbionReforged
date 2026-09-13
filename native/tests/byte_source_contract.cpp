#include "albion/archive.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>

using namespace albion::data;
using Bytes = std::vector<std::uint8_t>;
namespace {
int checks{};
void check(bool ok) { if (++checks && !ok) { std::cerr << "check " << checks << " failed\n"; std::exit(2); } }
struct ShortSource final : ByteSource {
    std::uint64_t size() const noexcept override { return 100; }
    Result<Bytes> ReadRange(std::uint64_t, std::uint64_t count) const override {
        return {Bytes(static_cast<std::size_t>(count ? count-1 : 0)), {}};
    }
};
}
int main(int argc, char** argv) {
    check(argc == 5);
    auto file = OpenFileSource(argv[1]); check(bool(file));
    const auto inner_size = std::stoull(argv[2]);
    check(!SliceSource({},0,0)); check(!Archive::Open(Source{}));
    check(!SliceSource(file.value,9,8));
    check(!SliceSource(file.value,0,std::numeric_limits<std::uint64_t>::max()));
    check(!file.value->ReadRange(std::numeric_limits<std::uint64_t>::max(),1));
    check(!file.value->ReadRange(1,std::numeric_limits<std::uint64_t>::max()));
    check(bool(file.value->ReadRange(file.value->size(),0)));
    check(!file.value->ReadRange(file.value->size()+1,0));
    auto limited = OpenFileSource(argv[1],8); check(bool(limited));
    check(limited.value->ReadRange(0,9).error.code == ErrorCode::limit);
    auto slice = SliceSource(file.value,17,17+inner_size); check(bool(slice));
    check(!slice.value->ReadRange(inner_size,1));
    auto empty = SliceSource(file.value,file.value->size(),file.value->size());
    check(bool(empty) && empty.value->size() == 0 && bool(empty.value->ReadRange(0,0)));
    auto short_parent = std::make_shared<ShortSource>();
    auto short_slice = SliceSource(short_parent,0,100); check(bool(short_slice));
    check(short_slice.value->ReadRange(0,10).error.code == ErrorCode::size_mismatch);
    check(Archive::Open(short_parent).error.code == ErrorCode::size_mismatch);
    auto inner = Archive::Open(slice.value); check(bool(inner));
    std::weak_ptr<const ByteSource> weak_file = file.value;
    file.value.reset(); slice.value.reset(); empty.value.reset();
    check(!weak_file.expired());
    auto outer = Archive::Open(argv[3]); check(bool(outer));
    std::shared_ptr<const Archive> owner(std::move(outer.value));
    check(owner->Read("same.bnk").error.code == ErrorCode::ambiguous);
    check(!ArchiveEntrySource({},0)); check(!ArchiveEntrySource(owner,99));
    auto entry = ArchiveEntrySource(owner,1); check(bool(entry));
    check(entry.value->size() == inner_size);
    auto nested_slice = SliceSource(entry.value,0,inner_size); check(bool(nested_slice));
    auto nested = Archive::Open(nested_slice.value); check(bool(nested));
    std::weak_ptr<const Archive> weak_outer = owner;
    owner.reset(); entry.value.reset(); nested_slice.value.reset();
    check(!weak_outer.expired());
    auto a = inner.value->Read(0); check(bool(a));
    auto b = nested.value->Read(0); check(bool(b) && a.value == b.value);
    std::ifstream expected_file(argv[4],std::ios::binary);
    Bytes expected{std::istreambuf_iterator<char>(expected_file),std::istreambuf_iterator<char>()};
    check(a.value == expected);
    auto concurrent = std::async(std::launch::async,[&] { return nested.value->ReadRange(0,31,4000); });
    auto range = nested.value->ReadRange(0,47,4096); check(bool(range));
    check(std::equal(range.value.begin(),range.value.end(),expected.begin()+47));
    auto other = concurrent.get(); check(bool(other));
    check(std::equal(other.value.begin(),other.value.end(),expected.begin()+31));
    inner.value.reset(); nested.value.reset();
    check(weak_file.expired() && weak_outer.expired());
    check(a.value == expected && b.value == expected);
    std::cout << checks << " source composition checks passed\n";
}
