#include <cstdio>
#include <string>

#include "solar/dom/Damage.h"
#include "solar/dom/Node.h"

namespace {

using namespace solar::dom;
using Quanta::Embed::Runtime;

int g_failed = 0;
int g_passed = 0;

void Check(bool condition, const char* what) {
  if (condition) {
    ++g_passed;
  } else {
    ++g_failed;
    std::printf("FAIL %s\n", what);
  }
}

std::string ErrorName(const std::optional<DomError>& error) { return error ? error->name : ""; }

void TestTree(Quanta::Context& ctx) {
  Document* document = NewDocument(ctx, true);
  Element* html = NewElement(ctx, document, "html");
  Element* body = NewElement(ctx, document, "body");
  Check(!AppendChild(document, html), "append the root element");
  Check(!AppendChild(html, body), "append a child");
  Check(document->DocumentElement() == html, "documentElement");
  Check(body->parentNode == html && html->firstChild == body && html->lastChild == body, "links of an only child");

  Element* a = NewElement(ctx, document, "a");
  Element* b = NewElement(ctx, document, "b");
  Element* c = NewElement(ctx, document, "c");
  AppendChild(body, a);
  AppendChild(body, c);
  Check(!PreInsert(b, body, c), "insertBefore");
  Check(a->nextSibling == b && b->nextSibling == c && c->previousSibling == b && body->lastChild == c, "sibling links after insertBefore");
  Check(b->IndexInParent() == 1 && body->ChildCount() == 3, "index and count");

  // Moving a node takes it out of where it was.
  Check(!AppendChild(body, a), "append moves");
  Check(body->firstChild == b && body->lastChild == a && a->previousSibling == c, "a moved to the end");
  Check(!PreInsert(b, body, b), "inserting a node before itself changes nothing");
  Check(body->firstChild == b && b->nextSibling == c, "still in place");

  Check(!RemoveChild(body, c), "removeChild");
  Check(c->parentNode == nullptr && b->nextSibling == a && a->previousSibling == b, "removed and relinked");
  Check(ErrorName(RemoveChild(body, c)) == "NotFoundError", "removing a non-child");

  Check(!ReplaceChild(body, c, b), "replaceChild");
  Check(body->firstChild == c && c->nextSibling == a && b->parentNode == nullptr, "replaced");
  Check(!ReplaceChild(body, a, a), "replacing a child with itself");
  Check(body->lastChild == a, "still there");
}

void TestValidity(Quanta::Context& ctx) {
  Document* document = NewDocument(ctx, true);
  Element* html = NewElement(ctx, document, "html");
  Element* body = NewElement(ctx, document, "body");
  AppendChild(document, html);
  AppendChild(html, body);
  Check(ErrorName(AppendChild(body, html)) == "HierarchyRequestError", "an ancestor cannot be inserted below");
  Check(ErrorName(AppendChild(body, body)) == "HierarchyRequestError", "a node cannot contain itself");
  Check(ErrorName(AppendChild(document, NewElement(ctx, document, "second"))) == "HierarchyRequestError", "a second document element");
  Check(ErrorName(AppendChild(document, NewText(ctx, document, "x"))) == "HierarchyRequestError", "text under a document");
  Check(ErrorName(AppendChild(body, document)) == "HierarchyRequestError", "a document is no child");
  Check(ErrorName(AppendChild(NewText(ctx, document, "t"), body)) == "HierarchyRequestError", "a text node has no children");
  Check(ErrorName(PreInsert(NewElement(ctx, document, "x"), body, html)) == "NotFoundError", "a reference child of another parent");

  DocumentType* doctype = NewDocumentType(ctx, document, "html", "", "");
  Check(ErrorName(AppendChild(document, doctype)) == "HierarchyRequestError", "a doctype after the element");
  Check(!PreInsert(doctype, document, html), "a doctype before the element");
  Check(document->Doctype() == doctype && document->firstChild == doctype, "doctype first");
  Check(ErrorName(PreInsert(NewDocumentType(ctx, document, "x", "", ""), document, html)) == "HierarchyRequestError", "a second doctype");
  Check(ErrorName(AppendChild(body, NewDocumentType(ctx, document, "x", "", ""))) == "HierarchyRequestError", "a doctype under an element");

  // A replacement that keeps the document valid.
  Check(!ReplaceChild(document, NewElement(ctx, document, "other"), html), "an element replaces the element");
  Check(document->DocumentElement() != html, "replaced");
}

void TestFragmentsAndAdoption(Quanta::Context& ctx) {
  Document* first = NewDocument(ctx, true);
  Document* second = NewDocument(ctx, true);
  Element* host = NewElement(ctx, first, "div");
  DocumentFragment* fragment = NewDocumentFragment(ctx, first);
  Element* one = NewElement(ctx, first, "i");
  Element* two = NewElement(ctx, first, "b");
  AppendChild(fragment, one);
  AppendChild(fragment, two);
  Check(!AppendChild(host, fragment), "a fragment is inserted");
  Check(host->ChildCount() == 2 && !fragment->HasChildNodes() && host->firstChild == one && host->lastChild == two, "its children moved, in order");

  Element* root = NewElement(ctx, first, "section");
  Element* child = NewElement(ctx, first, "p");
  SetAttribute(ctx, child, "id", "x");
  AppendChild(root, child);
  Document* intoSecond = second;
  AppendChild(intoSecond, NewElement(ctx, second, "html"));
  Element* target = intoSecond->DocumentElement();
  Check(!AppendChild(target, root), "inserted into another document");
  Check(root->nodeDocument == second && child->nodeDocument == second && child->attributes[0]->nodeDocument == second, "adopted with the whole subtree");
}

void TestText(Quanta::Context& ctx) {
  Document* document = NewDocument(ctx, true);
  Element* p = NewElement(ctx, document, "p");
  AppendChild(p, NewText(ctx, document, "Hello, "));
  Element* em = NewElement(ctx, document, "em");
  AppendChild(p, em);
  AppendChild(em, NewText(ctx, document, "world"));
  AppendChild(p, NewComment(ctx, document, "ignored"));
  AppendChild(p, NewText(ctx, document, "!"));
  Check(p->DescendantText() == "Hello, world!", "textContent skips comments");
  SetTextContent(ctx, p, "new");
  Check(p->ChildCount() == 1 && p->DescendantText() == "new", "setting textContent replaces the children");
  SetTextContent(ctx, p, "");
  Check(!p->HasChildNodes(), "and an empty one leaves none");
}

void TestAttributes(Quanta::Context& ctx) {
  Document* html = NewDocument(ctx, true);
  Document* xml = NewDocument(ctx, false);
  Element* div = NewElement(ctx, html, "div");
  Check(!SetAttribute(ctx, div, "ID", "a"), "set");
  Check(GetAttribute(div, "id") == "a", "an HTML element's attribute names are lowered");
  Check(GetAttribute(div, "Id") == "a", "and so are the names looked up");
  Check(!SetAttribute(ctx, div, "id", "b") && div->attributes.size() == 1 && GetAttribute(div, "id") == "b", "set again changes the value");
  Check(ErrorName(SetAttribute(ctx, div, "1bad", "x")) == "InvalidCharacterError", "a name cannot start with a digit");
  Check(ErrorName(SetAttribute(ctx, div, "a b", "x")) == "InvalidCharacterError", "a name has no space");
  Check(ErrorName(SetAttribute(ctx, div, "", "x")) == "InvalidCharacterError", "a name is not empty");
  Check(!SetAttribute(ctx, div, "data-x_y.z:w", "x"), "a name with the allowed punctuation");
  Check(RemoveAttribute(div, "ID") && !GetAttribute(div, "id"), "remove");
  Check(!RemoveAttribute(div, "id"), "removing what is not there");

  Element* x = NewElement(ctx, xml, "Div", "", "");
  SetAttribute(ctx, x, "Name", "v");
  Check(GetAttribute(x, "Name") == "v" && !GetAttribute(x, "name"), "an XML document keeps the case");
}

void TestCloneAndEquality(Quanta::Context& ctx) {
  Document* document = NewDocument(ctx, true);
  Element* root = NewElement(ctx, document, "ul");
  SetAttribute(ctx, root, "class", "list");
  for (const char* text : {"one", "two"}) {
    Element* item = NewElement(ctx, document, "li");
    AppendChild(item, NewText(ctx, document, text));
    AppendChild(root, item);
  }
  Node* deep = CloneNode(ctx, root, true);
  Node* shallow = CloneNode(ctx, root, false);
  Check(deep != root && IsEqualNode(deep, root), "a deep clone equals the original");
  Check(deep->ChildCount() == 2 && deep->firstChild != root->firstChild, "with children of its own");
  Check(!shallow->HasChildNodes() && !IsEqualNode(shallow, root), "a shallow one has none");
  Check(deep->nodeDocument == document && deep->parentNode == nullptr, "the clone is in the document and has no parent");
  SetAttribute(ctx, static_cast<Element*>(deep), "class", "other");
  Check(!IsEqualNode(deep, root) && GetAttribute(root, "class") == "list", "changing the clone leaves the original");
}

void TestPosition(Quanta::Context& ctx) {
  Document* document = NewDocument(ctx, true);
  Element* a = NewElement(ctx, document, "a");
  Element* b = NewElement(ctx, document, "b");
  Element* c = NewElement(ctx, document, "c");
  Element* d = NewElement(ctx, document, "d");
  AppendChild(document, a);
  AppendChild(a, b);
  AppendChild(a, c);
  AppendChild(b, d);
  Check(CompareDocumentPosition(b, c) == kFollowing, "c follows b");
  Check(CompareDocumentPosition(c, b) == kPreceding, "b precedes c");
  Check(CompareDocumentPosition(d, a) == (kContains | kPreceding), "a contains d");
  Check(CompareDocumentPosition(a, d) == (kContainedBy | kFollowing), "d is contained by a");
  Check(CompareDocumentPosition(d, c) == kFollowing, "c follows d, which is deeper before it");
  Element* lone = NewElement(ctx, document, "x");
  Check((CompareDocumentPosition(a, lone) & kDisconnected) != 0, "a node of no tree is disconnected");
  Check(CompareDocumentPosition(a, a) == 0, "a node and itself");
}

}  // namespace

void TestDamage(Quanta::Context& ctx) {
  Document* document = NewDocument(ctx, true);
  Element* html = NewElement(ctx, document, "html");
  Element* body = NewElement(ctx, document, "body");
  AppendChild(document, html);
  AppendChild(html, body);
  Check(DamageOf(body) == nullptr, "nothing is kept until asked for");
  DamageTracker& damage = EnableDamage(document);
  Element* div = NewElement(ctx, document, "div");
  AppendChild(body, div);
  CharacterData* text = NewText(ctx, document, "a");
  AppendChild(div, text);
  // A hundred changes to the same node are one entry.
  for (int i = 0; i < 100; ++i) SetCharacterData(text, std::to_string(i));
  Check(damage.NotedCount() >= 102, "every change is noted");
  Check(damage.PendingCount() == 3, "the two parents and the text, once each");
  Check(damage.Commit(document) && damage.Committed().size() == 3, "committed takes what was pending");
  Check(damage.PendingCount() == 0, "pending is empty after");
  // Changes during the work go to pending, and committing again waits for the work to be done.
  SetCharacterData(text, "later");
  Check(damage.PendingCount() == 1 && damage.Committed().size() == 3, "script goes on writing while committed is worked on");
  Check(damage.Commit(document) && damage.Committed().size() == 3 && damage.PendingCount() == 1, "no second commit before the first is finished");
  damage.Finish();
  Check(damage.Commit(document) && damage.Committed().size() == 1 && damage.Committed()[0].node == text, "then pending is committed");
  damage.Finish();
  // A node that leaves the tree is not worked on.
  SetCharacterData(text, "gone");
  RemoveChild(body, div);
  Check(!damage.Commit(document) || (damage.Committed().size() == 1 && damage.Committed()[0].node == body), "removed subtrees are forgotten");
}

int main() {
  auto runtime = Runtime::Create();
  Quanta::Context& ctx = runtime->GetContext();
  TestTree(ctx);
  TestValidity(ctx);
  TestFragmentsAndAdoption(ctx);
  TestText(ctx);
  TestAttributes(ctx);
  TestCloneAndEquality(ctx);
  TestPosition(ctx);
  TestDamage(ctx);
  std::printf("%d/%d passed\n", g_passed, g_passed + g_failed);
  return g_failed == 0 ? 0 : 1;
}
