const form = document.getElementById('navigation-form');
const addressBar = document.getElementById('address-bar');
const engineSelect = document.getElementById('engine-select');
const browserView = document.getElementById('browser-view');
const statusText = document.getElementById('status-text');
const tabRow = document.getElementById('tab-row');
const backButton = document.getElementById('back-button');
const forwardButton = document.getElementById('forward-button');
const newTabButton = document.getElementById('new-tab-button');

let tabs = [
  {
    id: 1,
    title: 'CodeOS Home',
    url: 'https://codeos.dev',
    history: ['https://codeos.dev'],
    historyIndex: 0,
  },
];
let activeTabId = 1;

function getActiveTab() {
  return tabs.find((tab) => tab.id === activeTabId) || tabs[0];
}

function renderTabs() {
  tabRow.innerHTML = '';
  tabs.forEach((tab) => {
    const button = document.createElement('button');
    button.className = `tab ${tab.id === activeTabId ? 'active' : ''}`;
    button.textContent = tab.title;
    button.addEventListener('click', () => {
      activeTabId = tab.id;
      renderTabs();
      syncAddressBar();
      updateView();
    });
    tabRow.appendChild(button);
  });

  const addButton = document.createElement('button');
  addButton.className = 'tab';
  addButton.textContent = '+ New Tab';
  addButton.addEventListener('click', createTab);
  tabRow.appendChild(addButton);
}

function syncAddressBar() {
  addressBar.value = getActiveTab().url;
}

function updateView() {
  const tab = getActiveTab();
  const target = encodeURIComponent(tab.url);
  browserView.src = `/render?target=${target}`;
  addressBar.value = tab.url;
}

function createTab(url = 'https://codeos.dev') {
  const tab = {
    id: Date.now(),
    title: 'New Tab',
    url,
    history: [url],
    historyIndex: 0,
  };
  tabs.push(tab);
  activeTabId = tab.id;
  renderTabs();
  syncAddressBar();
  updateView();
}

function updateHistory(url) {
  const tab = getActiveTab();
  if (tab.history[tab.historyIndex] !== url) {
    tab.history = tab.history.slice(0, tab.historyIndex + 1);
    tab.history.push(url);
    tab.historyIndex = tab.history.length - 1;
  }
}

function goBack() {
  const tab = getActiveTab();
  if (tab.historyIndex > 0) {
    tab.historyIndex -= 1;
    tab.url = tab.history[tab.historyIndex];
    syncAddressBar();
    updateView();
  }
}

function goForward() {
  const tab = getActiveTab();
  if (tab.historyIndex < tab.history.length - 1) {
    tab.historyIndex += 1;
    tab.url = tab.history[tab.historyIndex];
    syncAddressBar();
    updateView();
  }
}

form.addEventListener('submit', async (event) => {
  event.preventDefault();
  const query = addressBar.value.trim();
  if (!query) return;

  statusText.textContent = 'Navigating...';
  try {
    const response = await fetch('/search', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ query, engine: engineSelect.value }),
    });

    const payload = await response.json();
    const tab = getActiveTab();
    tab.url = payload.url;
    tab.title = payload.url.replace(/^https?:\/\//, '').split('/')[0] || 'OpenWeb';
    updateHistory(payload.url);
    renderTabs();
    syncAddressBar();
    updateView();
    statusText.textContent = `${payload.engine} • ${payload.mode}`;
  } catch (error) {
    statusText.textContent = `Navigation failed: ${error.message}`;
  }
});

backButton.addEventListener('click', goBack);
forwardButton.addEventListener('click', goForward);
newTabButton.addEventListener('click', () => createTab());
addressBar.addEventListener('focus', () => {
  addressBar.select();
});

renderTabs();
syncAddressBar();
updateView();
