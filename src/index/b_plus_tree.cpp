/**
 * b_plus_tree.cpp
 */
#include <iostream>

#include "common/exception.h"
#include "common/logger.h"
#include "common/rid.h"
#include "index/b_plus_tree.h"
#include "page/header_page.h"

namespace cmudb {

INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(const std::string &name,
                          BufferPoolManager *buffer_pool_manager,
                          const KeyComparator &comparator,
                          page_id_t root_page_id)
    : index_name_(name), root_page_id_(root_page_id),
      buffer_pool_manager_(buffer_pool_manager), comparator_(comparator) {}

/*
 * Helper function to decide whether current b+tree is empty
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::IsEmpty() const { return root_page_id_ == INVALID_PAGE_ID; }
/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::GetValue(const KeyType &key,
                              std::vector<ValueType> &result,
                              Transaction *transaction) {
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
bool BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value,
                            Transaction *transaction) {
  if (IsEmpty()) {
    StartNewTree(key, value);
    return true;
  }
  return InsertIntoLeaf(key, value, transaction);
}
/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 *
 *
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::StartNewTree(const KeyType &key, const ValueType &value) {
  assert(IsEmpty());
  page_id_t page_id;
  Page *page = buffer_pool_manager_->NewPage(page_id);
  if (page == nullptr) {
    throw std::bad_alloc{};
  }
  root_page_id_ = page_id;
  B_PLUS_TREE_LEAF_PAGE_TYPE *lp = reinterpret_cast<B_PLUS_TREE_LEAF_PAGE_TYPE *>(page->GetData());
  lp->Init(page_id, INVALID_PAGE_ID);
  InsertIntoLeaf(key, value);

  buffer_pool_manager_->UnpinPage(page_id, true);
}

/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immediately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::InsertIntoLeaf(const KeyType &key, const ValueType &value,
                                    Transaction *transaction) {
  page_id_t page_id = root_page_id_;
  Page *root = buffer_pool_manager_->FetchPage(page_id);
  BPlusTreePage *btp = reinterpret_cast<BPlusTreePage *>(root->GetData());
  while (!btp->IsLeafPage()) {
    BPInternalPage *ip = reinterpret_cast<BPInternalPage *>(btp);
    page_id_t next = ip->Lookup(key, comparator_);
    buffer_pool_manager_->UnpinPage(page_id, false);
    page_id = reinterpret_cast<page_id_t >(next);
    btp = reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(page_id)->GetData());
  }
  B_PLUS_TREE_LEAF_PAGE_TYPE *lp = reinterpret_cast<B_PLUS_TREE_LEAF_PAGE_TYPE *>(btp);
  auto originalSize = lp->GetSize();
  auto newSize = lp->Insert(key, value, comparator_);
  if (newSize > lp->GetMaxSize()) {
    B_PLUS_TREE_LEAF_PAGE_TYPE *oldlp = lp;
    B_PLUS_TREE_LEAF_PAGE_TYPE *newlp = Split(lp);
    InsertIntoParent(oldlp, oldlp->KeyAt(oldlp->GetSize() - 1), newlp);

    buffer_pool_manager_->UnpinPage(newlp->GetPageId(), true);
  }
  buffer_pool_manager_->UnpinPage(page_id, true);

  return originalSize != newSize;
}

/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page
 */
INDEX_TEMPLATE_ARGUMENTS
template<typename N>
N *BPLUSTREE_TYPE::Split(N *node) {
  page_id_t page_id;
  Page *newPage = buffer_pool_manager_->NewPage(page_id);
  if (newPage == nullptr) {
    throw std::bad_alloc();
  }
  typedef typename std::remove_pointer<N>::type *PagePtr;
  PagePtr ptr = reinterpret_cast<PagePtr>(newPage->GetData());
  ptr->Init(page_id, node->GetParentPageId());

  //this is different between leaf node and internal node.
  node->MoveHalfTo(ptr, buffer_pool_manager_);
  return ptr;
}

/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method
 * @param   key
 * @param   new_node      returned page from split() method
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertIntoParent(BPlusTreePage *old_node,
                                      const KeyType &key,
                                      BPlusTreePage *new_node,
                                      Transaction *transaction) {
  page_id_t parentPageId = old_node->GetParentPageId();
  BPInternalPage *ip = nullptr;
  if (parentPageId == INVALID_PAGE_ID) {
    Page *newPage = buffer_pool_manager_->NewPage(parentPageId);
    if (newPage == nullptr) {
      throw std::bad_alloc();
    }
    ip = reinterpret_cast<BPInternalPage *>(newPage->GetData());
    ip->Init(parentPageId, INVALID_PAGE_ID);
    root_page_id_ = parentPageId;
    UpdateRootPageId(false);
    old_node->SetParentPageId(parentPageId);
    new_node->SetParentPageId(parentPageId);
    ip->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());
//    ip->setKVAt(KeyType(), old_node->GetPageId(), 0);
//    ip->setKVAt(key, new_node->GetPageId(), 1);
    buffer_pool_manager_->UnpinPage(parentPageId, true);
    return;
  }

  ip = reinterpret_cast<BPInternalPage *>(buffer_pool_manager_->FetchPage(parentPageId)->GetData());

  //insert new kv pair points to new_node after that
  ip->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());

  if (ip->GetSize() > ip->GetMaxSize()) {
    BPInternalPage *oldlp = ip;
    BPInternalPage *newlp = Split(ip);
    InsertIntoParent(oldlp, newlp->KeyAt(0), newlp);
    buffer_pool_manager_->UnpinPage(newlp->GetPageId(), true);
  }
  buffer_pool_manager_->UnpinPage(parentPageId, true);
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
void BPLUSTREE_TYPE::Remove(const KeyType &key, Transaction *transaction) {
  if(IsEmpty()){
    return;
  }
  page_id_t page_id = root_page_id_;
  Page *root = buffer_pool_manager_->FetchPage(page_id);
  BPlusTreePage *btp = reinterpret_cast<BPlusTreePage *>(root->GetData());
  while (!btp->IsLeafPage()) {
    BPInternalPage *ip = reinterpret_cast<BPInternalPage *>(btp);
    page_id_t next = ip->Lookup(key, comparator_);
    buffer_pool_manager_->UnpinPage(page_id, false);
    page_id = next;
    btp = reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(page_id)->GetData());
  }

  B_PLUS_TREE_LEAF_PAGE_TYPE* lp = dynamic_cast<B_PLUS_TREE_LEAF_PAGE_TYPE*>(btp);
  auto size = lp->RemoveAndDeleteRecord(key, comparator_);
  if(size < lp->GetMinSize()){
    auto shouldRemovePage = CoalesceOrRedistribute(lp);
    if(shouldRemovePage){
      buffer_pool_manager_->UnpinPage(page_id, true);
      auto deletePage = buffer_pool_manager_->DeletePage(page_id);
      assert(deletePage);
    }
  }
  buffer_pool_manager_->UnpinPage(page_id, true);
}

/*
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
INDEX_TEMPLATE_ARGUMENTS
template<typename N>
bool BPLUSTREE_TYPE::CoalesceOrRedistribute(N *node, Transaction *transaction) {
  //node could be a leaf page and so a internal page
  BPlusTreePage* btp = node;
  BPInternalPage* parent = GetInternalPage(btp->GetParentPageId());
  auto idx = parent->ValueIndex(btp->GetPageId());

  BPlusTreePage * leftSibling = nullptr;
  BPlusTreePage * rightSibling = nullptr;
  //look ahead
  if(idx - 1 >= 0){
    //check size of the page
    leftSibling = GetInternalPage(parent->ValueAt(idx - 1));
    if(leftSibling->GetSize() > leftSibling->GetMinSize()){
      //redistribute with this page
      Redistribute(leftSibling, btp, 0);
      return false;
    }
  }

  if(idx + 1 < parent->GetSize()){
    //check size of the page
    rightSibling = GetInternalPage(parent->ValueAt(idx + 1));
    if(rightSibling->GetSize() > rightSibling->GetMinSize()){
      //redistribute with this page
      return false;
    }
  }

  assert(leftSibling || rightSibling);
  if(leftSibling){
    //merge with left sibling node
  }else{
    //merge with right sibling node
  }

  return true;
}

/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @return  true means parent node should be deleted, false means no deletion
 * happend
 */
INDEX_TEMPLATE_ARGUMENTS
template<typename N>
bool BPLUSTREE_TYPE::Coalesce(
    N *&neighbor_node, N *&node,
    BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *&parent,
    int index, Transaction *transaction) {
  return false;
}

/*
 * Redistribute key & value pairs from one page to its sibling page. If index ==
 * 0, move sibling page's first key & value pair into end of input "node",
 * otherwise move sibling page's last key & value pair into head of input
 * "node".
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 */
INDEX_TEMPLATE_ARGUMENTS
template<typename N>
void BPLUSTREE_TYPE::Redistribute(N *neighbor_node, N *node, int index) {
  BPInternalPage * neighbor = neighbor_node;
  BPInternalPage * cur = node;
  if(index == 0){
    neighbor->MoveFirstToEndOf(cur, buffer_pool_manager_);
  }else{
    neighbor->MoveLastToFrontOf(cur, buffer_pool_manager_);
  }
}
/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within coalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happend
 */
INDEX_TEMPLATE_ARGUMENTS
bool BPLUSTREE_TYPE::AdjustRoot(BPlusTreePage *old_root_node) {
  return false;
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the leaftmost leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE BPLUSTREE_TYPE::Begin() { return INDEXITERATOR_TYPE(); }

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
INDEX_TEMPLATE_ARGUMENTS
INDEXITERATOR_TYPE BPLUSTREE_TYPE::Begin(const KeyType &key) {
  return INDEXITERATOR_TYPE();
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*
 * Find leaf page containing particular key, if leftMost flag == true, find
 * the left most leaf page
 */
INDEX_TEMPLATE_ARGUMENTS
B_PLUS_TREE_LEAF_PAGE_TYPE *BPLUSTREE_TYPE::FindLeafPage(const KeyType &key,
                                                         bool leftMost) {
  return nullptr;
}

/*
 * Update/Insert root page id in header page(where page_id = 0, header_page is
 * defined under include/page/header_page.h)
 * Call this method every time root page id is changed.
 * @parameter: insert_record default value is false. When set to true,
 * insert a record <index_name, root_page_id> into header page instead of
 * updating it.
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::UpdateRootPageId(int insert_record) {
  HeaderPage *header_page = static_cast<HeaderPage *>(
      buffer_pool_manager_->FetchPage(HEADER_PAGE_ID));
  if (insert_record)
    // create a new record<index_name + root_page_id> in header_page
    header_page->InsertRecord(index_name_, root_page_id_);
  else
    // update root_page_id in header_page
    header_page->UpdateRecord(index_name_, root_page_id_);
  buffer_pool_manager_->UnpinPage(HEADER_PAGE_ID, true);
}

/*
 * This method is used for debug only
 * print out whole b+tree sturcture, rank by rank
 */
INDEX_TEMPLATE_ARGUMENTS
std::string BPLUSTREE_TYPE::ToString(bool verbose) { return "Empty tree"; }

/*
 * This method is used for test only
 * Read data from file and insert one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertFromFile(const std::string &file_name,
                                    Transaction *transaction) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;

    KeyType index_key;
    index_key.SetFromInteger(key);
    RID rid(key);
    Insert(index_key, rid, transaction);
  }
}
/*
 * This method is used for test only
 * Read data from file and remove one by one
 */
INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::RemoveFromFile(const std::string &file_name,
                                    Transaction *transaction) {
  int64_t key;
  std::ifstream input(file_name);
  while (input) {
    input >> key;
    KeyType index_key;
    index_key.SetFromInteger(key);
    Remove(index_key, transaction);
  }
}

template
class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;
template
class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;
template
class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;
template
class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;
template
class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

} // namespace cmudb
