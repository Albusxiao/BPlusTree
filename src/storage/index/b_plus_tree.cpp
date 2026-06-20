#include "storage/index/b_plus_tree.h"

#include <sstream>
#include <string>

#include "buffer/lru_k_replacer.h"
#include "common/config.h"
#include "common/exception.h"
#include "common/logger.h"
#include "common/macros.h"
#include "common/rid.h"
#include "storage/index/index_iterator.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/b_plus_tree_page.h"
#include "storage/page/page_guard.h"

namespace bustub
{

INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, page_id_t header_page_id,
                          BufferPoolManager* buffer_pool_manager,
                          const KeyComparator& comparator, int leaf_max_size,
                          int internal_max_size)
    : index_name_(std::move(name)),
      bpm_(buffer_pool_manager),
      comparator_(std::move(comparator)),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size),
      header_page_id_(header_page_id)
{
  WritePageGuard guard = bpm_ -> FetchPageWrite(header_page_id_);
  // In the original bpt, I fetch the header page
  // thus there's at least one page now
  auto root_header_page = guard.template AsMut<BPlusTreeHeaderPage>();
  // reinterprete the data of the page into "HeaderPage"
  root_header_page -> root_page_id_ = INVALID_PAGE_ID;
  // set the root_id to INVALID
}

/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const  ->  bool
{
  ReadPageGuard guard = bpm_ -> FetchPageRead(header_page_id_);
  auto root_header_page = guard.template As<BPlusTreeHeaderPage>();
  bool is_empty = root_header_page -> root_page_id_ == INVALID_PAGE_ID;
  // Just check if the root_page_id is INVALID
  // usage to fetch a page:
  // fetch the page guard   ->   call the "As" function of the page guard
  // to reinterprete the data of the page as "BPlusTreePage"
  return is_empty;
}
/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetValue(const KeyType& key,
                              std::vector<ValueType>* result, Transaction* txn)
     ->  bool
{
  ReadPageGuard header_guard = bpm_->FetchPageRead(header_page_id_);
  auto header_page = header_guard.template As<BPlusTreeHeaderPage>();
  if (header_page->root_page_id_ == INVALID_PAGE_ID)return false;
  ReadPageGuard guard = bpm_->FetchPageRead(header_page->root_page_id_);
  auto page = guard.template As<BPlusTreePage>();

  while (!page->IsLeafPage()) {
    auto internal = reinterpret_cast<const InternalPage*>(page);
    int slot = BinaryFind(internal, key);
    if (slot == -1) {
      return false;
    }
    page_id_t child_id = internal->ValueAt(slot);
    guard = bpm_->FetchPageRead(child_id);
    page = guard.template As<BPlusTreePage>();
  }
  auto leaf = reinterpret_cast<const LeafPage*>(page);
  int slot = BinaryFind(leaf, key);
  if (slot != -1 && comparator_(leaf->KeyAt(slot), key) == 0) {
    result->push_back(leaf->ValueAt(slot));
    return true;
  }
  return false;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */


INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType& key, const ValueType& value,
                            Transaction* txn)  ->  bool
{
  ReadPageGuard header_rguard = bpm_->FetchPageRead(header_page_id_);
  auto header_page_r = header_rguard.template As<BPlusTreeHeaderPage>();


  if (header_page_r->root_page_id_ == INVALID_PAGE_ID) {
    WritePageGuard header_wguard = bpm_->FetchPageWrite(header_page_id_);
    auto header_page = header_wguard.template AsMut<BPlusTreeHeaderPage>();
    page_id_t new_root_id;
    BasicPageGuard basic = bpm_->NewPageGuarded(&new_root_id);
    WritePageGuard root_guard = basic.UpgradeWrite();
    auto root_leaf = root_guard.template AsMut<LeafPage>();
    root_leaf->Init(leaf_max_size_);
    root_leaf->SetAt(0, key, value);
    root_leaf->SetSize(1);
    header_page->root_page_id_ = new_root_id;
    return true;
  }

  std::vector<std::pair<page_id_t, int>> path;
  ReadPageGuard guard = bpm_->FetchPageRead(header_page_r->root_page_id_);
  auto page = guard.template As<BPlusTreePage>();
  while (!page->IsLeafPage()) {
    auto internal = reinterpret_cast<const InternalPage*>(page);
    int slot = BinaryFind(internal, key);
    page_id_t child_id = internal->ValueAt(slot);
    path.emplace_back(guard.PageId(), slot);
    guard = bpm_->FetchPageRead(child_id);
    page = guard.template As<BPlusTreePage>();
  }
  page_id_t leaf_id = guard.PageId();
  WritePageGuard leaf_wguard = bpm_->FetchPageWrite(leaf_id);
  auto leaf = leaf_wguard.template AsMut<LeafPage>();

  int pos = BinaryFind(leaf, key);
  if (pos != -1 && comparator_(leaf->KeyAt(pos), key) == 0) {
    return false;
  }
   std::vector<std::pair<KeyType, ValueType>> entries;
   int n = leaf->GetSize();
   for (int i = 0; i < n; i++) {
     entries.emplace_back(leaf->KeyAt(i), leaf->ValueAt(i));
   }
   // Find insert position: skip all keys that are < key
   int insert_pos = 0;
   while (insert_pos < (int)entries.size() && comparator_(entries[insert_pos].first, key) < 0) {
     insert_pos++;
   }
   entries.insert(entries.begin() + insert_pos, {key, value});
   if ((int)entries.size() <= leaf_max_size_) {
     for (int i = 0; i < (int)entries.size(); i++) {
       leaf->SetAt(i, entries[i].first, entries[i].second);
     }
     leaf->SetSize((int)entries.size());
     return true;
   }
  int total = entries.size();
  int left_size = total / 2;
  int right_size = total - left_size;
  for (int i = 0; i < left_size; i++) {
    leaf->SetAt(i, entries[i].first, entries[i].second);
  }
  leaf->SetSize(left_size);
  page_id_t new_leaf_id;
  BasicPageGuard new_basic = bpm_->NewPageGuarded(&new_leaf_id);
  WritePageGuard new_leaf_wguard = new_basic.UpgradeWrite();
  auto new_leaf = new_leaf_wguard.template AsMut<LeafPage>();
  new_leaf->Init(leaf_max_size_);
  for (int i = 0; i < right_size; i++) {
    new_leaf->SetAt(i, entries[left_size + i].first, entries[left_size + i].second);
  }
  new_leaf->SetSize(right_size);
  new_leaf->SetNextPageId(leaf->GetNextPageId());
  leaf->SetNextPageId(new_leaf_id);
  KeyType sep_key = new_leaf->KeyAt(0);
  page_id_t child_id = new_leaf_id;
  for (int idx = (int)path.size() - 1; idx >= 0; idx--) {
    page_id_t parent_id = path[idx].first;
    int parent_slot = path[idx].second;
    WritePageGuard parent_wguard = bpm_->FetchPageWrite(parent_id);
    auto parent = parent_wguard.template AsMut<InternalPage>();
    std::vector<std::pair<KeyType, page_id_t>> p_entries;
    int psz = parent->GetSize();
    for (int i = 0; i < psz; i++) {
      p_entries.emplace_back(parent->KeyAt(i), parent->ValueAt(i));
    }
     int insert_pos_parent = parent_slot + 1;
     if (insert_pos_parent < 0) insert_pos_parent = 0;
     if (insert_pos_parent > (int)p_entries.size()) insert_pos_parent = p_entries.size();
     p_entries.insert(p_entries.begin() + insert_pos_parent, {sep_key, child_id});
     // if parent can hold
     if ((int)p_entries.size() <= internal_max_size_) {
       for (int i = 0; i < (int)p_entries.size(); i++) {
         parent->SetKeyAt(i, p_entries[i].first);
         parent->SetValueAt(i, p_entries[i].second);
       }
       parent->SetSize((int)p_entries.size());
       return true;
     }
    int totalp = p_entries.size();
    int mid = totalp / 2;
    page_id_t new_internal_id;
    BasicPageGuard nb = bpm_->NewPageGuarded(&new_internal_id);
    WritePageGuard new_internal_wguard = nb.UpgradeWrite();
    auto new_internal = new_internal_wguard.template AsMut<InternalPage>();
    new_internal->Init(internal_max_size_);
    int leftp = mid;
    int rightp = totalp - leftp;
    for (int i = 0; i < leftp; i++) {
      parent->SetKeyAt(i, p_entries[i].first);
      parent->SetValueAt(i, p_entries[i].second);
    }
    parent->SetSize(leftp);

    new_internal->SetSize(rightp);
    for (int i = 0; i < rightp; i++) {
      new_internal->SetKeyAt(i, p_entries[leftp + i].first);
      new_internal->SetValueAt(i, p_entries[leftp + i].second);
    }
    sep_key = new_internal->KeyAt(0);
    child_id = new_internal_id;
  }
  WritePageGuard header_wguard2 = bpm_->FetchPageWrite(header_page_id_);
  auto header_page2 = header_wguard2.template AsMut<BPlusTreeHeaderPage>();
  page_id_t new_root_id2;
  BasicPageGuard nbroot = bpm_->NewPageGuarded(&new_root_id2);
  WritePageGuard root_wguard = nbroot.UpgradeWrite();
  auto root_internal = root_wguard.template AsMut<InternalPage>();
  root_internal->Init(internal_max_size_);
  root_internal->SetValueAt(0, header_page_r->root_page_id_);
  root_internal->SetKeyAt(1, sep_key);
  root_internal->SetValueAt(1, child_id);
  root_internal->SetSize(2);
  header_page2->root_page_id_ = new_root_id2;
  return true;
}


/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType& key, Transaction* txn)
{
  ReadPageGuard header_rguard = bpm_->FetchPageRead(header_page_id_);
  auto header_page_r = header_rguard.As<BPlusTreeHeaderPage>();
  if (header_page_r->root_page_id_ == INVALID_PAGE_ID) {
    return;
  }
  std::vector<std::pair<page_id_t, int>> path;
  ReadPageGuard guard = bpm_->FetchPageRead(header_page_r->root_page_id_);
  auto page = guard.As<BPlusTreePage>();
  page_id_t root_id = header_page_r->root_page_id_;

  while (!page->IsLeafPage()) {
    auto internal = reinterpret_cast<const InternalPage*>(page);
    int slot = BinaryFind(internal, key);
    page_id_t child_id = internal->ValueAt(slot);
    path.emplace_back(guard.PageId(), slot);
    guard = bpm_->FetchPageRead(child_id);
    page = guard.As<BPlusTreePage>();
  }


  page_id_t leaf_id = guard.PageId();
  WritePageGuard leaf_wguard = bpm_->FetchPageWrite(leaf_id);
  auto leaf = leaf_wguard.AsMut<LeafPage>();

  int pos = BinaryFind(leaf, key);
  if (pos == -1 || comparator_(leaf->KeyAt(pos), key) != 0) {
    return;
  }

  int old_size = leaf->GetSize();
  for (int i = pos; i < old_size - 1; i++) {
    leaf->SetAt(i, leaf->KeyAt(i + 1), leaf->ValueAt(i + 1));
  }
  leaf->SetSize(old_size - 1);
  if (leaf->GetSize() >= leaf->GetMinSize()) {
    return;
  }
  if (leaf_id == root_id) {
    if (leaf->GetSize() == 0) {
      WritePageGuard header_wguard = bpm_->FetchPageWrite(header_page_id_);
      auto header_page = header_wguard.AsMut<BPlusTreeHeaderPage>();
      header_page->root_page_id_ = INVALID_PAGE_ID;
    }
    return;
  }
  page_id_t parent_id = path.back().first;
  int slot_in_parent = path.back().second;

  WritePageGuard parent_wguard = bpm_->FetchPageWrite(parent_id);
  auto parent = parent_wguard.AsMut<InternalPage>();
  if (slot_in_parent > 0) {
    page_id_t left_sibling_id = parent->ValueAt(slot_in_parent - 1);
    ReadPageGuard left_sibling_guard = bpm_->FetchPageRead(left_sibling_id);
    auto left_sibling = left_sibling_guard.As<LeafPage>();

    if (left_sibling->GetSize() > left_sibling->GetMinSize()) {
      int left_size = left_sibling->GetSize();
      KeyType borrowed_key = left_sibling->KeyAt(left_size - 1);
      ValueType borrowed_value = left_sibling->ValueAt(left_size - 1);
      for (int i = leaf->GetSize(); i > 0; i--) {
        leaf->SetAt(i, leaf->KeyAt(i - 1), leaf->ValueAt(i - 1));
      }
      leaf->SetAt(0, borrowed_key, borrowed_value);
      leaf->SetSize(leaf->GetSize() + 1);
      WritePageGuard left_sibling_wguard = bpm_->FetchPageWrite(left_sibling_id);
      auto left_sibling_mut = left_sibling_wguard.AsMut<LeafPage>();
      left_sibling_mut->SetSize(left_size - 1);
      parent->SetKeyAt(slot_in_parent, leaf->KeyAt(0));
      return;
    }
  }

  if (slot_in_parent < parent->GetSize() - 1) {
    page_id_t right_sibling_id = parent->ValueAt(slot_in_parent + 1);
    ReadPageGuard right_sibling_guard = bpm_->FetchPageRead(right_sibling_id);
    auto right_sibling = right_sibling_guard.As<LeafPage>();

    if (right_sibling->GetSize() > right_sibling->GetMinSize()) {
      KeyType borrowed_key = right_sibling->KeyAt(0);
      ValueType borrowed_value = right_sibling->ValueAt(0);
      leaf->SetAt(leaf->GetSize(), borrowed_key, borrowed_value);
      leaf->SetSize(leaf->GetSize() + 1);
      WritePageGuard right_sibling_wguard = bpm_->FetchPageWrite(right_sibling_id);
      auto right_sibling_mut = right_sibling_wguard.AsMut<LeafPage>();
      int rsize = right_sibling_mut->GetSize();
      for (int i = 0; i < rsize - 1; i++) {
        right_sibling_mut->SetAt(i, right_sibling_mut->KeyAt(i + 1), right_sibling_mut->ValueAt(i + 1));
      }
      right_sibling_mut->SetSize(rsize - 1);
      parent->SetKeyAt(slot_in_parent + 1, right_sibling_mut->KeyAt(0));
      return;
    }
  }
  if (slot_in_parent > 0) {
    page_id_t left_sibling_id = parent->ValueAt(slot_in_parent - 1);
    WritePageGuard left_sibling_wguard = bpm_->FetchPageWrite(left_sibling_id);
    auto left_sibling = left_sibling_wguard.AsMut<LeafPage>();
    int left_size = left_sibling->GetSize();
    int curr_size = leaf->GetSize();
    for (int i = 0; i < curr_size; i++) {
      left_sibling->SetAt(left_size + i, leaf->KeyAt(i), leaf->ValueAt(i));
    }
    left_sibling->SetSize(left_size + curr_size);
    left_sibling->SetNextPageId(leaf->GetNextPageId());
    for (int i = slot_in_parent; i < parent->GetSize() - 1; i++) {
      parent->SetKeyAt(i, parent->KeyAt(i + 1));
      parent->SetValueAt(i, parent->ValueAt(i + 1));
    }
    parent->SetSize(parent->GetSize() - 1);
  } else {
    page_id_t right_sibling_id = parent->ValueAt(slot_in_parent + 1);
    WritePageGuard right_sibling_wguard = bpm_->FetchPageWrite(right_sibling_id);
    auto right_sibling = right_sibling_wguard.AsMut<LeafPage>();
    int curr_size = leaf->GetSize();
    int right_size = right_sibling->GetSize();
    for (int i = 0; i < right_size; i++) {
      leaf->SetAt(curr_size + i, right_sibling->KeyAt(i), right_sibling->ValueAt(i));
    }
    leaf->SetSize(curr_size + right_size);
    leaf->SetNextPageId(right_sibling->GetNextPageId());
    for (int i = slot_in_parent + 1; i < parent->GetSize() - 1; i++) {
      parent->SetKeyAt(i, parent->KeyAt(i + 1));
      parent->SetValueAt(i, parent->ValueAt(i + 1));
    }
    parent->SetSize(parent->GetSize() - 1);
  }
  if (parent->GetSize() >= parent->GetMinSize()) {
    return;
  }
  if (parent_id == root_id) {
    if (parent->GetSize() == 1) {
      page_id_t new_root_id = parent->ValueAt(0);
      WritePageGuard header_wguard = bpm_->FetchPageWrite(header_page_id_);
      auto header_page = header_wguard.AsMut<BPlusTreeHeaderPage>();
      header_page->root_page_id_ = new_root_id;
    }
    return;
  }
  path.pop_back();
  if (path.empty()) {
    page_id_t grandparent_id = root_id;
    ReadPageGuard grandparent_guard = bpm_->FetchPageRead(grandparent_id);
    auto grandparent = grandparent_guard.As<BPlusTreePage>();
    if (grandparent->IsLeafPage()) {
      return;
    }
    auto grandparent_internal = reinterpret_cast<const InternalPage*>(grandparent);
    int gp_slot = -1;
    for (int i = 0; i < grandparent_internal->GetSize(); i++) {
      if (grandparent_internal->ValueAt(i) == parent_id) {
        gp_slot = i;
        break;
      }
    }

    if (gp_slot == -1) return;
  }
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/


INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::BinaryFind(const LeafPage* leaf_page, const KeyType& key)
     ->  int
{
  int l = 0;
  int r = leaf_page -> GetSize() - 1;
  while (l < r)
  {
    int mid = (l + r + 1) >> 1;
    if (comparator_(leaf_page -> KeyAt(mid), key) != 1)
    {
      l = mid;
    }
    else
    {
      r = mid - 1;
    }
  }

  if (r >= 0 && comparator_(leaf_page -> KeyAt(r), key) == 1)
  {
    r = -1;
  }

  return r;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::BinaryFind(const InternalPage* internal_page,
                                const KeyType& key)  ->  int
{
  int l = 1;
  int r = internal_page -> GetSize() - 1;
  while (l < r)
  {
    int mid = (l + r + 1) >> 1;
    if (comparator_(internal_page -> KeyAt(mid), key) != 1)
    {
      l = mid;
    }
    else
    {
      r = mid - 1;
    }
  }

  if (r == -1 || comparator_(internal_page -> KeyAt(r), key) == 1)
  {
    r = 0;
  }

  return r;
}

/*
 * Input parameter is void, find the leftmost leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin()  ->  INDEXITERATOR_TYPE
//Just go left forever
{
  ReadPageGuard head_guard = bpm_ -> FetchPageRead(header_page_id_);
  if (head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_ == INVALID_PAGE_ID)
  {
    return End();
  }
  ReadPageGuard guard = bpm_ -> FetchPageRead(head_guard.As<BPlusTreeHeaderPage>() -> root_page_id_);
  head_guard.Drop();

  auto tmp_page = guard.template As<BPlusTreePage>();
  while (!tmp_page -> IsLeafPage())
  {
    int slot_num = 0;
    guard = bpm_ -> FetchPageRead(reinterpret_cast<const InternalPage*>(tmp_page) -> ValueAt(slot_num));
    tmp_page = guard.template As<BPlusTreePage>();
  }
  int slot_num = 0;
  if (slot_num != -1)
  {
    return INDEXITERATOR_TYPE(bpm_, guard.PageId(), 0);
  }
  return End();
}


/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType& key)  ->  INDEXITERATOR_TYPE
{
  ReadPageGuard head_guard = bpm_ -> FetchPageRead(header_page_id_);

  if (head_guard.template As<BPlusTreeHeaderPage>() -> root_page_id_ == INVALID_PAGE_ID)
  {
    return End();
  }
  ReadPageGuard guard = bpm_ -> FetchPageRead(head_guard.As<BPlusTreeHeaderPage>() -> root_page_id_);
  head_guard.Drop();
  auto tmp_page = guard.template As<BPlusTreePage>();
  while (!tmp_page -> IsLeafPage())
  {
    auto internal = reinterpret_cast<const InternalPage*>(tmp_page);
    int slot_num = BinaryFind(internal, key);
    if (slot_num == -1)
    {
      return End();
    }
    guard = bpm_ -> FetchPageRead(reinterpret_cast<const InternalPage*>(tmp_page) -> ValueAt(slot_num));
    tmp_page = guard.template As<BPlusTreePage>();
  }
  auto* leaf_page = reinterpret_cast<const LeafPage*>(tmp_page);

  int slot_num = BinaryFind(leaf_page, key);
  if (slot_num != -1)
  {
    return INDEXITERATOR_TYPE(bpm_, guard.PageId(), slot_num);
  }
  return End();
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End()  ->  INDEXITERATOR_TYPE
{
  return INDEXITERATOR_TYPE(bpm_, -1, -1);
}

/**
 * @return Page id of the root of this tree
 */
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId()  ->  page_id_t
{
  ReadPageGuard guard = bpm_ -> FetchPageRead(header_page_id_);
  auto root_header_page = guard.template As<BPlusTreeHeaderPage>();
  page_id_t root_page_id = root_header_page -> root_page_id_;
  return root_page_id;
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/

/*
 * This method is used for test only
 * Read data from file and insert one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertFromFile(const std::string& file_name,
                                    Transaction* txn)
{
  int64_t key;
  std::ifstream input(file_name);
  while (input >> key)
  {
    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    Insert(index_key, rid, txn);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string& file_name,
                                    Transaction* txn)
{
  int64_t key;
  std::ifstream input(file_name);
  while (input >> key)
  {
    KeyType index_key;
    index_key.SetFromInteger(key);
    Remove(index_key, txn);
  }
}

/*
 * This method is used for test only
 * Read data from file and insert/remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::BatchOpsFromFile(const std::string& file_name,
                                      Transaction* txn)
{
  int64_t key;
  char instruction;
  std::ifstream input(file_name);
  while (input)
  {
    input >> instruction >> key;
    RID rid(key);
    KeyType index_key;
    index_key.SetFromInteger(key);
    switch (instruction)
    {
      case 'i':
        Insert(index_key, rid, txn);
        break;
      case 'd':
        Remove(index_key, txn);
        break;
      default:
        break;
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Print(BufferPoolManager* bpm)
{
  auto root_page_id = GetRootPageId();
  auto guard = bpm -> FetchPageBasic(root_page_id);
  PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::PrintTree(page_id_t page_id, const BPlusTreePage* page)
{
  if (page -> IsLeafPage())
  {
    auto* leaf = reinterpret_cast<const LeafPage*>(page);
    std::cout << "Leaf Page: " << page_id << "\tNext: " << leaf -> GetNextPageId() << std::endl;

    // Print the contents of the leaf page.
    std::cout << "Contents: ";
    for (int i = 0; i < leaf -> GetSize(); i++)
    {
      std::cout << leaf -> KeyAt(i);
      if ((i + 1) < leaf -> GetSize())
      {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
  }
  else
  {
    auto* internal = reinterpret_cast<const InternalPage*>(page);
    std::cout << "Internal Page: " << page_id << std::endl;

    // Print the contents of the internal page.
    std::cout << "Contents: ";
    for (int i = 0; i < internal -> GetSize(); i++)
    {
      std::cout << internal -> KeyAt(i) << ": " << internal -> ValueAt(i);
      if ((i + 1) < internal -> GetSize())
      {
        std::cout << ", ";
      }
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal -> GetSize(); i++)
    {
      auto guard = bpm_ -> FetchPageBasic(internal -> ValueAt(i));
      PrintTree(guard.PageId(), guard.template As<BPlusTreePage>());
    }
  }
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Draw(BufferPoolManager* bpm, const std::string& outf)
{
  if (IsEmpty())
  {
    LOG_WARN("Drawing an empty tree");
    return;
  }

  std::ofstream out(outf);
  out << "digraph G {" << std::endl;
  auto root_page_id = GetRootPageId();
  auto guard = bpm -> FetchPageBasic(root_page_id);
  ToGraph(guard.PageId(), guard.template As<BPlusTreePage>(), out);
  out << "}" << std::endl;
  out.close();
}

/**
 * This method is used for debug only, You don't need to modify
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::ToGraph(page_id_t page_id, const BPlusTreePage* page,
                             std::ofstream& out)
{
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page -> IsLeafPage())
  {
    auto* leaf = reinterpret_cast<const LeafPage*>(page);
    // Print node name
    out << leaf_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" "
           "CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf -> GetSize() << "\">P=" << page_id
        << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf -> GetSize() << "\">"
        << "max_size=" << leaf -> GetMaxSize()
        << ",min_size=" << leaf -> GetMinSize() << ",size=" << leaf -> GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf -> GetSize(); i++)
    {
      out << "<TD>" << leaf -> KeyAt(i) << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf -> GetNextPageId() != INVALID_PAGE_ID)
    {
      out << leaf_prefix << page_id << "   ->   " << leaf_prefix
          << leaf -> GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << page_id << " " << leaf_prefix
          << leaf -> GetNextPageId() << "};\n";
    }
  }
  else
  {
    auto* inner = reinterpret_cast<const InternalPage*>(page);
    // Print node name
    out << internal_prefix << page_id;
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" "
           "CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner -> GetSize() << "\">P=" << page_id
        << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner -> GetSize() << "\">"
        << "max_size=" << inner -> GetMaxSize()
        << ",min_size=" << inner -> GetMinSize() << ",size=" << inner -> GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner -> GetSize(); i++)
    {
      out << "<TD PORT=\"p" << inner -> ValueAt(i) << "\">";
      // if (i > 0) {
      out << inner -> KeyAt(i) << "  " << inner -> ValueAt(i);
      // } else {
      // out << inner  ->  ValueAt(0);
      // }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print leaves
    for (int i = 0; i < inner -> GetSize(); i++)
    {
      auto child_guard = bpm_ -> FetchPageBasic(inner -> ValueAt(i));
      auto child_page = child_guard.template As<BPlusTreePage>();
      ToGraph(child_guard.PageId(), child_page, out);
      if (i > 0)
      {
        auto sibling_guard = bpm_ -> FetchPageBasic(inner -> ValueAt(i - 1));
        auto sibling_page = sibling_guard.template As<BPlusTreePage>();
        if (!sibling_page -> IsLeafPage() && !child_page -> IsLeafPage())
        {
          out << "{rank=same " << internal_prefix << sibling_guard.PageId()
              << " " << internal_prefix << child_guard.PageId() << "};\n";
        }
      }
      out << internal_prefix << page_id << ":p" << child_guard.PageId()
          << "   ->   ";
      if (child_page -> IsLeafPage())
      {
        out << leaf_prefix << child_guard.PageId() << ";\n";
      }
      else
      {
        out << internal_prefix << child_guard.PageId() << ";\n";
      }
    }
  }
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::DrawBPlusTree()  ->  std::string
{
  if (IsEmpty())
  {
    return "()";
  }

  PrintableBPlusTree p_root = ToPrintableBPlusTree(GetRootPageId());
  std::ostringstream out_buf;
  p_root.Print(out_buf);

  return out_buf.str();
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::ToPrintableBPlusTree(page_id_t root_id)
     ->  PrintableBPlusTree
{
  auto root_page_guard = bpm_ -> FetchPageBasic(root_id);
  auto root_page = root_page_guard.template As<BPlusTreePage>();
  PrintableBPlusTree proot;

  if (root_page -> IsLeafPage())
  {
    auto leaf_page = root_page_guard.template As<LeafPage>();
    proot.keys_ = leaf_page -> ToString();
    proot.size_ = proot.keys_.size() + 4;  // 4 more spaces for indent

    return proot;
  }

  // draw internal page
  auto internal_page = root_page_guard.template As<InternalPage>();
  proot.keys_ = internal_page -> ToString();
  proot.size_ = 0;
  for (int i = 0; i < internal_page -> GetSize(); i++)
  {
    page_id_t child_id = internal_page -> ValueAt(i);
    PrintableBPlusTree child_node = ToPrintableBPlusTree(child_id);
    proot.size_ += child_node.size_;
    proot.children_.push_back(child_node);
  }

  return proot;
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;

template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;

template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;

template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;

template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub